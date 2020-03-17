//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************

#include <D3Dcompiler.h>
#include <random>
#include <sstream>
#include <ppl.h>

#include "Compute.h"
#include "Render.h" // for struct Particle
#include "ExtensionHelper.h" // Intel extensions

const float Compute::ParticleSpread = PARTICLE_SPREAD;

enum ComputeRootParameters : UINT32
{
    ComputeRootCBV = 0,
    ComputeRootUAVTable,
    ComputeRootParametersCount
};

struct ConstantBufferCS
{
    UINT param[4];
    float paramf[4];
};

// Indices of shader resources in the descriptor heap.
enum DescriptorHeapIndex : UINT32
{
    UavParticlePos0 = 0, // u0
    UavParticlePos1,
    UavParticlePos0Copy, // so we can ping-pong just by moving the heap base

    UavParticleVel0, // u3
    UavParticleVel1,
    UavParticleVel0Copy, // so we can ping-pong just by moving the heap base
    DescriptorCount
};

enum class GpuTimers
{
    Simulate,
    NumTimers
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// FIXME: shared functions go in a header?
extern std::wstring GetAssetFullPath(const std::wstring in_filename);

//-----------------------------------------------------------------------------
// creates a command queue with the intel extension if available
//-----------------------------------------------------------------------------
void Compute::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    m_commandQueue.Reset();
    if (m_usingIntelCommandQueueExtension)
    {
        m_commandQueue = m_pExtensionHelper->CreateCommandQueue(desc);
    }
    else
    {
        ThrowIfFailed(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue)));
    }
}

//-----------------------------------------------------------------------------
// Creates a command queue optionally using the intel throttle extension
// NOTE: the GPU must be idle at this point
//-----------------------------------------------------------------------------
void Compute::SetUseIntelCommandQueueExtension(bool in_desiredSetting)
{
    in_desiredSetting = in_desiredSetting && m_pExtensionHelper->GetEnabled();
    if (m_usingIntelCommandQueueExtension != in_desiredSetting)
    {
        m_usingIntelCommandQueueExtension = in_desiredSetting;
        CreateCommandQueue();
    }
}

//-----------------------------------------------------------------------------
// Create two buffers in the GPU, each with a copy of the particles data
// The compute shader reads from one and writes to the other
//-----------------------------------------------------------------------------
void Compute::CreateSharedBuffers()
{
    /*
    Shared Heap notes from https://docs.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps

    Cross adapter shared heaps enable multiple adapters to share data without the CPU marshaling the data between them.
    Cross-adapter shared resources are only supported in system memory.
    */

    /*
    Why aren't we using D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS?
    MSDN:
    Use of this flag can compromise resource fences to perform waits, and prevents any compression being used with a resource.
    Cannot be used with D3D12_RESOURCE_DIMENSION_BUFFER; but buffers always have the properties represented by this flag.
    */

    const UINT dataSize = m_numParticles * sizeof(Render::Particle);

    D3D12_RESOURCE_DESC crossAdapterDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    D3D12_RESOURCE_ALLOCATION_INFO textureInfo =
        m_device->GetResourceAllocationInfo(0, 1, &crossAdapterDesc);

    UINT64 alignedDataSize = textureInfo.SizeInBytes;

    CD3DX12_HEAP_DESC heapDesc(
        m_NUM_BUFFERS * alignedDataSize,
        D3D12_HEAP_TYPE_DEFAULT,
        0, // An alias for 64KB. See documentation for D3D12_HEAP_DESC
        D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);

    ThrowIfFailed(m_device->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_sharedHeap)));

    ThrowIfFailed(m_device->CreateSharedHandle(m_sharedHeap.Get(), nullptr,
        GENERIC_ALL, 0/*L"SHARED_HEAP"*/, &m_sharedHandles.m_heap));

    m_sharedHandles.m_alignedDataSize = alignedDataSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = m_numParticles;
    uavDesc.Buffer.StructureByteStride = sizeof(Render::Particle);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    D3D12_UNORDERED_ACCESS_VIEW_DESC velocityDesc = uavDesc;
    velocityDesc.Buffer.StructureByteStride = sizeof(ParticleVelocity);

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        ThrowIfFailed(m_device->CreatePlacedResource(
            m_sharedHeap.Get(),
            i * alignedDataSize,
            &crossAdapterDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&m_positionBuffers[i])));

        CD3DX12_CPU_DESCRIPTOR_HANDLE heapHandle(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            UavParticlePos0 + i, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_positionBuffers[i].Get(),
            nullptr, &uavDesc, heapHandle);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(m_numParticles * sizeof(ParticleVelocity), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&m_velocityBuffers[i])));

        CD3DX12_CPU_DESCRIPTOR_HANDLE velHeapHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), UavParticleVel0 + i, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_velocityBuffers[i].Get(), nullptr, &velocityDesc, velHeapHandle);
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE copyPosHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), UavParticlePos0Copy, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(m_positionBuffers[0].Get(), nullptr, &uavDesc, copyPosHandle);

    CD3DX12_CPU_DESCRIPTOR_HANDLE copyVelHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), UavParticleVel0Copy, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(m_velocityBuffers[0].Get(), nullptr, &velocityDesc, copyVelHandle);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Compute::Compute(UINT in_numParticles,
    Microsoft::WRL::ComPtr<IDXGIAdapter1> in_adapter,
    bool in_useIntelCommandQueueExtension,
    Compute* in_pCompute) :
    m_numParticles(in_numParticles),
    m_frameFenceValues{}
{
    m_bufferIndex = 0;
    m_usingIntelCommandQueueExtension = in_useIntelCommandQueueExtension;
    m_fenceValue = 0;

    Initialize(in_adapter);

    if (in_pCompute)
    {
        CopyState(in_pCompute);
    }
    else
    {
        InitializeParticles();
    }

    WaitForGpu();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Compute::~Compute()
{
    WaitForGpu();
    delete m_pExtensionHelper;

    CloseHandle(m_sharedHandles.m_heap);
    CloseHandle(m_sharedHandles.m_fence);
}

//-----------------------------------------------------------------------------
// cross-adapter copy from other compute object into this one
//-----------------------------------------------------------------------------
void Compute::CopyState(Compute* in_pCompute)
{
    //---------------------------------------------------------------
    // open shared buffers
    //---------------------------------------------------------------
    ComPtr<ID3D12Heap> sharedHeap;
    m_device->OpenSharedHandle(in_pCompute->m_sharedHandles.m_heap, IID_PPV_ARGS(&sharedHeap));

    D3D12_RESOURCE_DESC crossAdapterDesc = CD3DX12_RESOURCE_DESC::Buffer(
        in_pCompute->m_sharedHandles.m_alignedDataSize,
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    ComPtr<ID3D12Resource> srcBuffer[m_NUM_BUFFERS];

    for (int i = 0; i < m_NUM_BUFFERS; i++)
    {
        ThrowIfFailed(m_device->CreatePlacedResource(
            sharedHeap.Get(),
            i * in_pCompute->m_sharedHandles.m_alignedDataSize,
            &crossAdapterDesc,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            nullptr,
            IID_PPV_ARGS(&srcBuffer[i])));
    }

    //---------------------------------------------------------------
    // copy the position data from the other compute device
    //---------------------------------------------------------------
    m_commandAllocators[m_bufferIndex]->Reset();
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    for (int i = 0; i < m_NUM_BUFFERS; i++)
    {
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));

        m_commandList->CopyBufferRegion(
            m_positionBuffers[i].Get(), 0,
            srcBuffer[i].Get(), 0,
            m_numParticles * sizeof(Render::Particle));

        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
    }

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);

    // wait for this to complete
    WaitForGpu();

    //---------------------------------------------------------------
    // within the other adapter, copy the velocity buffers into the shared position buffers
    // WARNING: the size of the velocity data better be <= the size of the position data
    //---------------------------------------------------------------
    {
        in_pCompute->m_commandAllocators[m_bufferIndex]->Reset();
        ThrowIfFailed(in_pCompute->m_commandList->Reset(in_pCompute->m_commandAllocators[m_bufferIndex].Get(), in_pCompute->m_computeState.Get()));

        for (int i = 0; i < m_NUM_BUFFERS; i++)
        {
            in_pCompute->m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(in_pCompute->m_positionBuffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));
            in_pCompute->m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(in_pCompute->m_velocityBuffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));


            in_pCompute->m_commandList->CopyBufferRegion(
                in_pCompute->m_positionBuffers[i].Get(), 0,
                in_pCompute->m_velocityBuffers[i].Get(), 0,
                m_numParticles * sizeof(ParticleVelocity));
        }
        ThrowIfFailed(in_pCompute->m_commandList->Close());
        ID3D12CommandList* ppCommandLists[] = { in_pCompute->m_commandList.Get() };
        in_pCompute->m_commandQueue->ExecuteCommandLists(1, ppCommandLists);

        in_pCompute->WaitForGpu();
    }

    m_commandAllocators[m_bufferIndex]->Reset();
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    for (int i = 0; i < m_NUM_BUFFERS; i++)
    {
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));

        m_commandList->CopyBufferRegion(
            m_velocityBuffers[i].Get(), 0,
            srcBuffer[i].Get(), 0,
            m_numParticles * sizeof(ParticleVelocity));

        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }

    {
        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
    }

    WaitForGpu();
}

//-----------------------------------------------------------------------------
// called to dynamically change the compute adapter
//-----------------------------------------------------------------------------
void Compute::SetAdapter(ComPtr<IDXGIAdapter1> in_adapter)
{
    m_adapter = in_adapter;

    CreateDevice(m_adapter.Get(), m_device);

    m_pExtensionHelper = new ExtensionHelper(m_device.Get());
    m_usingIntelCommandQueueExtension = m_usingIntelCommandQueueExtension && m_pExtensionHelper->GetEnabled();
    CreateCommandQueue();

    for (int i = 0; i < m_NUM_BUFFERS; i++)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_commandAllocators[i])));
        std::wstringstream cmdAllocName;
        cmdAllocName << "Compute CmdAlloc " << i;
        m_commandAllocators[i]->SetName(cmdAllocName.str().c_str());
    }

    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    m_commandList->SetName(L"ComputeCommandList");

    ThrowIfFailed(m_device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&m_fence)));
    ThrowIfFailed(m_device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, 0/*L"COMPUTE_FENCE"*/, &m_sharedHandles.m_fence));
    m_fenceValue++;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }

    // create timer on the command queue
    delete m_pTimer;
    m_pTimer = new D3D12GpuTimer(m_device.Get(), m_commandQueue.Get(), static_cast<UINT>(GpuTimers::NumTimers));
    m_pTimer->SetTimerName(static_cast<UINT>(GpuTimers::Simulate), "simulate ms");
}

//-----------------------------------------------------------------------------
// create root sig, pipeline state, descriptor heap, srv uav cbv
//-----------------------------------------------------------------------------
void Compute::Initialize(ComPtr<IDXGIAdapter1> in_adapter)
{
    SetAdapter(in_adapter);

    m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Compute root signature.
    {
        // one UAV range of 2 registers, u0 and u1
        CD3DX12_DESCRIPTOR_RANGE1 uavRanges[] = {
            CD3DX12_DESCRIPTOR_RANGE1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE)
        };

        CD3DX12_ROOT_PARAMETER1 rootParameters[ComputeRootParametersCount] = {};
        rootParameters[ComputeRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[ComputeRootUAVTable].InitAsDescriptorTable(_countof(uavRanges), uavRanges, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
        computeRootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr);

        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

        // This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
        NAME_D3D12_OBJECT(m_rootSignature);
    }

    // Create the pipeline states, which includes compiling and loading shaders.
    {
        ComPtr<ID3DBlob> computeShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        // Load and compile shaders.
        D3D_SHADER_MACRO macros[] = { { "blocksize", STRINGIFY(BLOCK_SIZE) }, { NULL, NULL} };
        ID3DBlob* pErrorMsgs = 0;

        const wchar_t* pShaderName = L"NBodyGravityCS.hlsl";

        //ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(pShaderName).c_str(), macros, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &computeShader, &pErrorMsgs));
        HRESULT hr = D3DCompileFromFile(GetAssetFullPath(pShaderName).c_str(), macros, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &computeShader, &pErrorMsgs);
        char* pMessage = 0;
        if (FAILED(hr))
        {
            pMessage = (char*)pErrorMsgs->GetBufferPointer();
            ThrowIfFailed(hr);
        }
        // Describe and create the compute pipeline state object (PSO).
        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = m_rootSignature.Get();
        computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

        ThrowIfFailed(m_device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&m_computeState)));
        NAME_D3D12_OBJECT(m_computeState);
    }

    // Note: ComPtrs are CPU objects but this resource needs to stay in scope until
    // the command list that references it has finished executing on the GPU.
    // We will flush the GPU at the end of this method to ensure the resource is not
    // prematurely destroyed.
    ComPtr<ID3D12Resource> constantBufferCSUpload;

    // Create the compute shader's constant buffer.
    {
        const UINT bufferSize = sizeof(ConstantBufferCS);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferCS)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constantBufferCSUpload)));

        NAME_D3D12_OBJECT(m_constantBufferCS);

        ConstantBufferCS constantBufferCS = {};
        constantBufferCS.param[0] = m_numParticles;
        constantBufferCS.param[1] = int(ceil(m_numParticles / float(BLOCK_SIZE)));
        constantBufferCS.paramf[0] = 0.1f;
        constantBufferCS.paramf[1] = 1.0f;

        D3D12_SUBRESOURCE_DATA computeCBData = {};
        computeCBData.pData = reinterpret_cast<UINT8*>(&constantBufferCS);
        computeCBData.RowPitch = bufferSize;
        computeCBData.SlicePitch = computeCBData.RowPitch;

        UpdateSubresources<1>(m_commandList.Get(), m_constantBufferCS.Get(), constantBufferCSUpload.Get(), 0, 0, 1, &computeCBData);
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_constantBufferCS.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    }

    // close command buffer & execute to initialize gpu resources
    m_commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    WaitForGpu();

    D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
    srvUavHeapDesc.NumDescriptors = DescriptorCount;
    srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    CreateSharedBuffers();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void LoadParticles(
    _Out_writes_(numParticles) Render::Particle* out_pParticles,
    _Out_writes_(numParticles) Compute::ParticleVelocity* out_pVelocities,
    const XMFLOAT3& center, const float initialSpeed, float spread, UINT numParticles)
{
    std::random_device randomDevice;  //Will be used to obtain a seed for the random number engine
    std::mt19937 gen(randomDevice()); //Standard mersenne_twister_engine seeded with rd()
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    concurrency::parallel_for(UINT(0), numParticles, [&](UINT i)
        {
            XMVECTOR delta = XMVectorSet(dist(gen), dist(gen), dist(gen), 0);
            while (XMVectorGetX(XMVector3LengthSq(delta)) < 10.0f)
            {
                delta += XMVectorSet(dist(gen), dist(gen), dist(gen), 0);
            }
            delta = XMVector3Normalize(delta) * spread;

            out_pParticles[i].position.x = center.x + XMVectorGetX(delta);
            out_pParticles[i].position.y = center.y + XMVectorGetY(delta);
            out_pParticles[i].position.z = center.z + XMVectorGetZ(delta);

            // create a velocity perpindicular-ish to the direction to the center of gravity
            XMVECTOR direction = XMVector3NormalizeEst(XMLoadFloat3((XMFLOAT3*)&out_pParticles[i].position));
            XMVECTOR perp = XMVector3NormalizeEst(XMVectorSubtract(XMVectorSet(1, 1, 1, 0), direction));
            XMVECTOR vel = XMVectorScale(XMVector3Cross(direction, perp), initialSpeed);

            out_pVelocities[i].velocity.x = XMVectorGetX(vel);
            out_pVelocities[i].velocity.y = XMVectorGetY(vel);
            out_pVelocities[i].velocity.z = XMVectorGetZ(vel);
    });
}

//-----------------------------------------------------------------------------
// initialize particle positions.
// Only need to do this once.
// on subsequent compute destroy/create, can copy old state from render object
//-----------------------------------------------------------------------------
void Compute::InitializeParticles()
{
    // Initialize the data in the buffers.
    std::vector<Render::Particle> positions;
    positions.resize(m_numParticles);

    std::vector<ParticleVelocity> velocities;
    velocities.resize(m_numParticles);

    // Split the particles into two groups.
    float centerSpread = ParticleSpread * 0.750f;
    LoadParticles(
        &positions[0], &velocities[0],
        XMFLOAT3(centerSpread, 0, 0),
        INITIAL_PARTICLE_SPEED,
        ParticleSpread,
        m_numParticles / 2);
    LoadParticles(
        &positions[m_numParticles / 2], &velocities[m_numParticles / 2],
        XMFLOAT3(-centerSpread, 0, 0),
        INITIAL_PARTICLE_SPEED,
        ParticleSpread,
        m_numParticles / 2);

    //-------------------------------------------------------------------------
    // upload positions
    //-------------------------------------------------------------------------
    const UINT dataSize = m_numParticles * sizeof(Render::Particle);
    D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

    ComPtr<ID3D12Resource> particleBufferUpload;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&particleBufferUpload)));

    D3D12_SUBRESOURCE_DATA particleData = {};
    particleData.pData = reinterpret_cast<UINT8*>(&positions[0]);
    particleData.RowPitch = dataSize;
    particleData.SlicePitch = particleData.RowPitch;

    ID3D12GraphicsCommandList* pCommandList = m_commandList.Get();
    m_commandAllocators[m_bufferIndex]->Reset();
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    {
        CD3DX12_RESOURCE_BARRIER barriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[1].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        m_commandList->ResourceBarrier(_countof(barriers), barriers);
    }

    UpdateSubresources<1>(m_commandList.Get(), m_positionBuffers[0].Get(), particleBufferUpload.Get(), 0, 0, 1, &particleData);
    UpdateSubresources<1>(m_commandList.Get(), m_positionBuffers[1].Get(), particleBufferUpload.Get(), 0, 0, 1, &particleData);

    //-------------------------------------------------------------------------
    // upload velocities
    //-------------------------------------------------------------------------
    UINT64 velocityBufferSize = m_numParticles * sizeof(ParticleVelocity);
    D3D12_RESOURCE_DESC velocityBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(velocityBufferSize);
    ComPtr<ID3D12Resource> velocityBufferUpload;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &velocityBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&velocityBufferUpload)));

    D3D12_SUBRESOURCE_DATA velocityData = {};
    particleData.pData = reinterpret_cast<UINT8*>(&velocities[0]);
    particleData.RowPitch = velocityBufferSize;
    particleData.SlicePitch = particleData.RowPitch;

    UpdateSubresources<1>(m_commandList.Get(), m_velocityBuffers[0].Get(), velocityBufferUpload.Get(), 0, 0, 1, &particleData);
    UpdateSubresources<1>(m_commandList.Get(), m_velocityBuffers[1].Get(), velocityBufferUpload.Get(), 0, 0, 1, &particleData);

    {
        CD3DX12_RESOURCE_BARRIER barriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        m_commandList->ResourceBarrier(_countof(barriers), barriers);
    }

    ThrowIfFailed(pCommandList->Close());
    ID3D12CommandList* ppCommandLists[] = { pCommandList };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
    WaitForGpu();
}

//-----------------------------------------------------------------------------
// NOTE: UNUSED with multi-gpu
//-----------------------------------------------------------------------------
void Compute::WaitForGpu()
{
    // Add a signal command to the queue.
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue));

    // Instruct the fence to set the event object when the signal command completes.
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
    m_fenceValue++;

    // Wait until the signal command has been processed.
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Compute::SharedHandles Compute::GetSharedHandles(HANDLE in_fenceHandle)
{
    m_device->OpenSharedHandle(in_fenceHandle, IID_PPV_ARGS(&m_sharedFence));

    m_sharedHandles.m_bufferIndex = m_bufferIndex;
    return m_sharedHandles;
}

//-----------------------------------------------------------------------------
// signal frame is complete, move to next fence value
// FIXME: expects there to only be 2 buffers
//-----------------------------------------------------------------------------
void Compute::MoveToNextFrame()
{
    // Assign the current fence value to the current frame.
    m_frameFenceValues[m_bufferIndex] = m_fenceValue;

    // Signal and increment the fence value.
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue));
    m_fenceValue++;

    // Update the frame index.
    m_bufferIndex = 1 - m_bufferIndex;
}

//-----------------------------------------------------------------------------
// Run the particle simulation using the compute shader.
//-----------------------------------------------------------------------------
void Compute::Simulate(int in_numActiveParticles, UINT64 in_sharedFenceValue)
{
    // /previous/ copy must complete before overwriting the old state
    ThrowIfFailed(m_commandQueue->Wait(m_sharedFence.Get(), in_sharedFenceValue-1));

    UINT oldIndex = m_bufferIndex; // 0 or 1. Old corresponds to the surface the render device is currently using
    UINT newIndex = 1 - oldIndex;  // 1 or 0. New corresponds to the surface the render device is NOT using

    m_commandAllocators[m_bufferIndex]->Reset();
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    m_pTimer->BeginTimer(m_commandList.Get(), static_cast<std::uint32_t>(GpuTimers::Simulate));

    UINT srcHeapIndex = UavParticlePos0 + oldIndex; // 0 or 1

    ID3D12DescriptorHeap* ppHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    m_commandList->SetPipelineState(m_computeState.Get());
    m_commandList->SetComputeRootSignature(m_rootSignature.Get());
    m_commandList->SetComputeRootConstantBufferView(ComputeRootCBV, m_constantBufferCS->GetGPUVirtualAddress());

    //-------------------------------------------------
    // set heap base to point at previous simulation results
    // note that descriptor heap[2] is a copy of heap[0], so when the base is heap[1] the dest is heap[2]==heap[0]
    //-------------------------------------------------
    CD3DX12_GPU_DESCRIPTOR_HANDLE srcHeapHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), srcHeapIndex, m_srvUavDescriptorSize);
    m_commandList->SetComputeRootDescriptorTable(ComputeRootUAVTable, srcHeapHandle);

    //-------------------------------------------------
    // dispatch reads from src and writes to dest
    //-------------------------------------------------
    ID3D12Resource* pSharedResource = m_positionBuffers[newIndex].Get();
    m_commandList->Dispatch(static_cast<UINT>(ceil(in_numActiveParticles / float(BLOCK_SIZE))), 1, 1);

    // a resource barrier gives maximum information to the runtime that may help other adapters with cache sync
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(pSharedResource));

    m_pTimer->EndTimer(m_commandList.Get(), static_cast<std::uint32_t>(GpuTimers::Simulate));
    m_pTimer->ResolveAllTimers(m_commandList.Get());

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);

    MoveToNextFrame();
}
