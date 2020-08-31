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

#include <cassert>
#include <random>
#include <string>
#include <sstream>
#include <ppl.h>
#include <D3Dcompiler.h>

#include "Compute.h"
#include "Render.h" // for struct Particle
#include "ExtensionHelper.h" // Intel extensions

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
Compute::Compute(UINT in_numParticles,
    IDXGIAdapter1* in_pAdapter,
    bool in_useIntelCommandQueueExtension,
    Compute* in_pCompute)
    : m_numParticles(in_numParticles)
    , m_pExtensionHelper(nullptr)
    , m_srvUavDescriptorSize(0)
    , m_fenceEvent(nullptr)
    , m_bufferIndex(0)
    , m_frameFenceValues{}
    , m_fenceValue(0)
{
    m_usingIntelCommandQueueExtension = in_useIntelCommandQueueExtension;

    Initialize(in_pAdapter);

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

    if (m_usingIntelCommandQueueExtension)
    {
        // INTC extension seems to internally increase ref count.
        // Can't use ComPtr<T>::Reset() here!
        m_commandQueue->Release();
    }

    BOOL rv = ::CloseHandle(m_sharedHandles.m_heap);
    assert(rv != FALSE);

    rv = ::CloseHandle(m_sharedHandles.m_fence);
    assert(rv != FALSE);

    rv = ::CloseHandle(m_fenceEvent);
    assert(rv != FALSE);
}

//-----------------------------------------------------------------------------
// creates a command queue with the intel extension if available
//-----------------------------------------------------------------------------
void Compute::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

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
    WaitForGpu();
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

    const D3D12_RESOURCE_DESC crossAdapterDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    const D3D12_RESOURCE_ALLOCATION_INFO textureInfo =
        m_device->GetResourceAllocationInfo(0, 1, &crossAdapterDesc);

    const UINT64 alignedDataSize = textureInfo.SizeInBytes;

    const CD3DX12_HEAP_DESC heapDesc(
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

        const CD3DX12_CPU_DESCRIPTOR_HANDLE heapHandle(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            UavParticlePos0 + i,
            m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_positionBuffers[i].Get(), nullptr, &uavDesc, heapHandle);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(m_numParticles * sizeof(ParticleVelocity), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&m_velocityBuffers[i])));

        const CD3DX12_CPU_DESCRIPTOR_HANDLE velHeapHandle(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            UavParticleVel0 + i,
            m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_velocityBuffers[i].Get(), nullptr, &velocityDesc, velHeapHandle);
    }

    const CD3DX12_CPU_DESCRIPTOR_HANDLE copyPosHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), UavParticlePos0Copy, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(m_positionBuffers[0].Get(), nullptr, &uavDesc, copyPosHandle);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE copyVelHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), UavParticleVel0Copy, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(m_velocityBuffers[0].Get(), nullptr, &velocityDesc, copyVelHandle);
}

//-----------------------------------------------------------------------------
// when we create a compute device for async compute, we compute directly into
// the buffers used for rendering and abandon our reference to the shared resources.
// if a new compute device is created that is not async compute, we need to
// copy the current particle positions into the old shared buffers before we
// copy the data into the new compute object.
//-----------------------------------------------------------------------------
void Compute::ResetFromAsyncHelper()
{
    // if the reference copy made for async matches my current reference,
    // then we are not running in async mode and no copy is necessary.
    if (m_positionBuffers[0] == m_sharedComputeBuffersReference[0])
    {
        return;
    }

    ThrowIfFailed(m_commandAllocators[m_bufferIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        auto src = m_positionBuffers[i].Get();
        auto dst = m_sharedComputeBuffersReference[i].Get();

        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));

        m_commandList->CopyBufferRegion(
            dst, 0,
            src, 0,
            m_numParticles * sizeof(Render::Particle));

        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);

    WaitForGpu();

    // reset the old references
    SetAsync(m_sharedRenderFence, m_sharedComputeBuffersReference, m_bufferIndex);
}

//-----------------------------------------------------------------------------
// cross-adapter copy from other compute object into this one
//-----------------------------------------------------------------------------
void Compute::CopyState(Compute* in_pCompute)
{
    in_pCompute->ResetFromAsyncHelper();

    //---------------------------------------------------------------
    // open shared buffers
    //---------------------------------------------------------------
    ComPtr<ID3D12Heap> sharedHeap;
    ThrowIfFailed(m_device->OpenSharedHandle(in_pCompute->m_sharedHandles.m_heap, IID_PPV_ARGS(&sharedHeap)));

    const D3D12_RESOURCE_DESC crossAdapterDesc = CD3DX12_RESOURCE_DESC::Buffer(
        in_pCompute->m_sharedHandles.m_alignedDataSize,
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    ComPtr<ID3D12Resource> srcBuffer[m_NUM_BUFFERS];

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
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
    {
        ThrowIfFailed(m_commandAllocators[m_bufferIndex]->Reset());
        ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

        for (UINT i = 0; i < m_NUM_BUFFERS; i++)
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

        WaitForGpu();
    }

    //---------------------------------------------------------------
    // within the other adapter, copy the velocity buffers into the shared position buffers
    // WARNING: the size of the velocity data better be <= the size of the position data
    //---------------------------------------------------------------
    {
        in_pCompute->m_commandAllocators[m_bufferIndex]->Reset();
        ThrowIfFailed(in_pCompute->m_commandList->Reset(in_pCompute->m_commandAllocators[m_bufferIndex].Get(), in_pCompute->m_computeState.Get()));

        for (UINT i = 0; i < m_NUM_BUFFERS; i++)
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

    //---------------------------------------------------------------
    // now copy the velocity data from the other compute device
    //---------------------------------------------------------------
    {
        ThrowIfFailed(m_commandAllocators[m_bufferIndex]->Reset());
        ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

        for (UINT i = 0; i < m_NUM_BUFFERS; i++)
        {
            m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));

            m_commandList->CopyBufferRegion(
                m_velocityBuffers[i].Get(), 0,
                srcBuffer[i].Get(), 0,
                m_numParticles * sizeof(ParticleVelocity));

            m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        }

        ThrowIfFailed(m_commandList->Close());

        ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
    }

    WaitForGpu();
}

//-----------------------------------------------------------------------------
// create root sig, pipeline state, descriptor heap, srv uav cbv
//-----------------------------------------------------------------------------
void Compute::Initialize(IDXGIAdapter1* in_pAdapter)
{
    CreateDevice(in_pAdapter, m_device);

    m_pExtensionHelper = new ExtensionHelper(m_device.Get());
    m_usingIntelCommandQueueExtension = m_usingIntelCommandQueueExtension && m_pExtensionHelper->GetEnabled();
    CreateCommandQueue();

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_commandAllocators[i])));
        std::wostringstream cmdAllocName;
        cmdAllocName << "Compute CmdAlloc " << i;
        m_commandAllocators[i]->SetName(cmdAllocName.str().c_str());
    }

    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    m_commandList->SetName(L"ComputeCommandList");

    ThrowIfFailed(m_device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&m_fence)));
    ThrowIfFailed(m_device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, 0/*L"COMPUTE_FENCE"*/, &m_sharedHandles.m_fence));
    m_fenceValue++;

    m_fenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }

    // create timer on the command queue
    m_pTimer = new D3D12GpuTimer(m_device.Get(), m_commandQueue.Get(), static_cast<UINT>(GpuTimers::NumTimers));
    m_pTimer->SetTimerName(static_cast<UINT>(GpuTimers::Simulate), "simulate ms");

    m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Compute root signature.
    {
        // one UAV range of 2 registers, u0 and u1
        const CD3DX12_DESCRIPTOR_RANGE1 uavRanges[] = {
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
        // Load and compile shaders.
        ID3DBlob* pErrorMsgs = nullptr;
        ComPtr<ID3DBlob> computeShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        const UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        const UINT compileFlags = 0;
#endif

        const D3D_SHADER_MACRO macros[] = { { "blocksize", STRINGIFY(BLOCK_SIZE) }, { nullptr, nullptr} };

        const wchar_t* pShaderName = L"NBodyGravityCS.hlsl";
        const std::wstring fullShaderPath = GetAssetFullPath(pShaderName);

        const HRESULT hr = ::D3DCompileFromFile(fullShaderPath.c_str(), macros, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &computeShader, &pErrorMsgs);
        if (FAILED(hr))
        {
            if (pErrorMsgs != nullptr)
            {
                const char* pMessage = (const char*)pErrorMsgs->GetBufferPointer();
                ::OutputDebugStringA(pMessage);
                pErrorMsgs->Release();
            }            
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
    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    WaitForGpu();

    D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
    srvUavHeapDesc.NumDescriptors = DescriptorCount;
    srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    CreateSharedBuffers();

    // shenanigans to simplify transitioning /out/ of async compute mode:
    // keep a 2nd reference to these shared resources so we can copy stuff through them to a new compute object
    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        m_sharedComputeBuffersReference[i] = m_positionBuffers[i];
    }
}

#define USE_ORIG 1
#define USE_SCALAR_OPTIMIZED 0
#define USE_SIMD_OPTIMIZED 0

#if (USE_ORIG + USE_SCALAR_OPTIMIZED + USE_SIMD_OPTIMIZED) != 1
#error "use one of the options"
#endif

#define FAST_SIMD_RAND_COMPATABILITY
//#define BENCHMARK

//-----------------------------------------------------------------------------
// fast rand version
// see https://software.intel.com/en-us/articles/fast-random-number-generator-on-the-intel-pentiumr-4-processor
//-----------------------------------------------------------------------------
thread_local
unsigned int g_seed = 0;

inline void fast_srand(int seed)
{
    g_seed = seed;
}

// returns one integer, similar output value range as C lib
inline int fast_rand()
{
    g_seed = 214013 * g_seed + 2531011;
    return (g_seed >> 16) & 0x7FFF;
}


//-----------------------------------------------------------------------------
// faster SIMD rand version
// see https://software.intel.com/en-us/articles/fast-random-number-generator-on-the-intel-pentiumr-4-processor
//-----------------------------------------------------------------------------

#include <emmintrin.h>

thread_local
__m128i g_simd_seed;

void srand_sse(unsigned int seed)
{
    g_simd_seed = _mm_set_epi32(seed, seed + 1, seed, seed + 1);
}

inline __m128i __vectorcall  rand_sse()
{
    __declspec(align(16)) /*static*/ const unsigned int mult[4]   = { 214013, 17405, 214013, 69069 };
    __declspec(align(16)) /*static*/ const unsigned int gadd[4]   = { 2531011, 10395331, 13737667, 1 };
    __declspec(align(16)) /*static*/ const unsigned int mask[4]   = { 0xFFFFFFFF, 0, 0xFFFFFFFF, 0 };
    __declspec(align(16)) /*static*/ const unsigned int masklo[4] = { 0x00007FFF, 0x00007FFF, 0x00007FFF, 0x00007FFF };

    const __m128i adder = _mm_load_si128((__m128i*) gadd);
    __m128i multiplier = _mm_load_si128((__m128i*) mult);
    const __m128i mod_mask = _mm_load_si128((__m128i*) mask);
    const __m128i sra_mask = _mm_load_si128((__m128i*) masklo);
    __m128i cur_seed_split = _mm_shuffle_epi32(g_simd_seed, _MM_SHUFFLE(2, 3, 0, 1));

    g_simd_seed = _mm_mul_epu32(g_simd_seed, multiplier);
    multiplier = _mm_shuffle_epi32(multiplier, _MM_SHUFFLE(2, 3, 0, 1));
    cur_seed_split = _mm_mul_epu32(cur_seed_split, multiplier);
    g_simd_seed = _mm_and_si128(g_simd_seed, mod_mask);
    cur_seed_split = _mm_and_si128(cur_seed_split, mod_mask);
    cur_seed_split = _mm_shuffle_epi32(cur_seed_split, _MM_SHUFFLE(2, 3, 0, 1));
    g_simd_seed = _mm_or_si128(g_simd_seed, cur_seed_split);
    g_simd_seed = _mm_add_epi32(g_simd_seed, adder);

#if defined(FAST_SIMD_RAND_COMPATABILITY)

    // Add the lines below if you wish to reduce your results to 16-bit vals...
    __m128i sseresult = _mm_srai_epi32(g_simd_seed, 16);
    sseresult = _mm_and_si128(sseresult, sra_mask);
    return sseresult;

#else

    return g_simd_seed;

#endif
}

#include "Timer.h"

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void LoadParticles(
    _Out_writes_(numParticles) Render::Particle* out_pParticles,
    _Out_writes_(numParticles) Compute::ParticleVelocity* out_pVelocities,
    const XMFLOAT3& center, const float initialSpeed, float spread, UINT numParticles)
{
#ifdef BENCHMARK
    Timer t;
    t.Start();
#endif

#if USE_ORIG == 1
    // those are not thread safe
    std::random_device randomDevice;  //Will be used to obtain a seed for the random number engine
    std::mt19937 gen(randomDevice()); //Standard mersenne_twister_engine seeded with rd()
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
#endif

    concurrency::parallel_for(UINT(0), numParticles, [&](UINT i)
        {
#if USE_ORIG == 1

            // "original version"

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

#elif USE_SCALAR_OPTIMIZED == 1

            // "first batch of optimizations"
            // -using fast rand
            // -hoisting out scalar factor for 1 multiplication
            // -don't use very inefficient loads and stores
            // -SSA form
            // -const

            // collapse all multiplication factors into one
            // floating point accuracy / determinism (order of ops) aside, this is good enough for this workload
            constexpr float k_scale = (1.f / (float)RAND_MAX) * 2.f;

            float x = ((float)fast_rand() * k_scale) - 1.f;
            float y = ((float)fast_rand() * k_scale) - 1.f;
            float z = ((float)fast_rand() * k_scale) - 1.f;

            XMVECTOR delta = XMVectorSet(x, y, z, 0.f);
            while (XMVectorGetX(XMVector3LengthSq(delta)) < 10.f)
            {
                x = ((float)fast_rand() * k_scale) - 1.f;
                y = ((float)fast_rand() * k_scale) - 1.f;
                z = ((float)fast_rand() * k_scale) - 1.f;

                const XMVECTOR random = XMVectorSet(x, y, z, 0.f);
                delta = XMVectorAdd(delta, random);
            }

            delta = XMVector3Normalize(delta);
            delta = XMVectorScale(delta, spread);

            const XMVECTOR centerSimd = XMLoadFloat3(&center);
            const XMVECTOR position = XMVectorAdd(centerSimd, delta);
            XMStoreFloat4(&out_pParticles[i].position, position);

            // create a velocity perpindicular-ish to the direction to the center of gravity
            const XMVECTOR direction = XMVector3NormalizeEst(XMLoadFloat3((XMFLOAT3*)&out_pParticles[i].position));
            const XMVECTOR perp = XMVector3NormalizeEst(XMVectorSubtract(XMVectorSet(1.f, 1.f, 1.f, 0.f), direction));
            const XMVECTOR vel = XMVectorScale(XMVector3Cross(direction, perp), initialSpeed);
            XMStoreFloat3(&out_pVelocities[i].velocity, vel);

#elif USE_SIMD_OPTIMIZED == 1

            const XMVECTOR limit = XMVectorSet(10.f, 10.f, 10.f, 10.f);
            XMVECTOR delta = g_XMZero;
            XMVECTOR deltaLengthSq = g_XMZero;

            // collapse all multiplication factors into one
            // floating point accuracy / determinism (order of ops) aside, this is good enough for this workload
            constexpr float k_scale = (1.f / (float)RAND_MAX) * 2.f;

            const __m128 k_scale_v = _mm_set_ps(k_scale, k_scale, k_scale, k_scale);
            const __m128 k_one = _mm_set_ps(1.f, 1.f, 1.f, 1.f);

            do
            {
                const __m128i rand_vi = rand_sse();
                __m128 rand_vf = _mm_cvtepi32_ps(rand_vi);

                rand_vf = _mm_mul_ps(rand_vf, k_scale_v);
                rand_vf = _mm_sub_ps(rand_vf, k_one);

                const XMVECTOR random = rand_vf;

                delta = XMVectorAdd(delta, random);
                deltaLengthSq = XMVector3LengthSq(delta);
            }
            while (XMVector3Less(deltaLengthSq, limit));

            delta = XMVector3Normalize(delta);
            delta = XMVectorScale(delta, spread);

            const XMVECTOR centerSimd = XMLoadFloat3(&center);

            const XMVECTOR position = XMVectorAdd(centerSimd, delta);
            XMStoreFloat4(&out_pParticles[i].position, position);

            // create a velocity perpindicular-ish to the direction to the center of gravity
            const XMVECTOR direction = XMVector3NormalizeEst(XMLoadFloat3((XMFLOAT3*)&out_pParticles[i].position));
            const XMVECTOR perp = XMVector3NormalizeEst(XMVectorSubtract(XMVectorSet(1.f, 1.f, 1.f, 0.f), direction));
            const XMVECTOR vel = XMVectorScale(XMVector3Cross(direction, perp), initialSpeed);
            XMStoreFloat3(&out_pVelocities[i].velocity, vel);

#endif
    });

#ifdef BENCHMARK
    t.Stop();

    char buffer[255];
    sprintf_s(buffer, 255, "\nLoad Particles: %s %f\n", 
#if USE_ORIG == 1
        "USE_OLD_ORIG",
#elif USE_SCALAR_OPTIMIZED == 1
        "USE_SCALAR_OPTIMIZED",
#elif USE_SIMD_OPTIMIZED == 1
        "USE_SIMD_OPTIMIZED",
#endif
        t.GetTime());

    ::OutputDebugStringA(buffer);
#endif
}


//-----------------------------------------------------------------------------
// initialize particle positions.
// Only need to do this once.
// on subsequent compute destroy/create, can copy old state from render object
//-----------------------------------------------------------------------------
void Compute::InitializeParticles()
{
    assert(m_numParticles != 0);

    // Initialize the data in the buffers.
    std::vector<Render::Particle> positions;
    positions.resize(m_numParticles);

    std::vector<ParticleVelocity> velocities;
    velocities.resize(m_numParticles);

    // Split the particles into two groups.
    const float centerSpread = ParticleSpread * 0.750f;
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
    const D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

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

    ThrowIfFailed(m_commandAllocators[m_bufferIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    {
        const CD3DX12_RESOURCE_BARRIER barriers[] =
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
    const UINT64 velocityBufferSize = m_numParticles * sizeof(ParticleVelocity);
    const D3D12_RESOURCE_DESC velocityBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(velocityBufferSize);

    ComPtr<ID3D12Resource> velocityBufferUpload;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &velocityBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&velocityBufferUpload)));

    particleData.pData = reinterpret_cast<UINT8*>(&velocities[0]);
    particleData.RowPitch = velocityBufferSize;
    particleData.SlicePitch = particleData.RowPitch;

    UpdateSubresources<1>(m_commandList.Get(), m_velocityBuffers[0].Get(), velocityBufferUpload.Get(), 0, 0, 1, &particleData);
    UpdateSubresources<1>(m_commandList.Get(), m_velocityBuffers[1].Get(), velocityBufferUpload.Get(), 0, 0, 1, &particleData);

    {
        const CD3DX12_RESOURCE_BARRIER barriers[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_positionBuffers[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(m_velocityBuffers[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        m_commandList->ResourceBarrier(_countof(barriers), barriers);
    }

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
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
    const DWORD rv = ::WaitForSingleObject(m_fenceEvent, INFINITE);
    assert(rv == WAIT_OBJECT_0);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
const Compute::SharedHandles& Compute::GetSharedHandles(HANDLE in_fenceHandle)
{
    ThrowIfFailed(m_device->OpenSharedHandle(in_fenceHandle, IID_PPV_ARGS(&m_sharedRenderFence)));

    m_sharedHandles.m_bufferIndex = m_bufferIndex;
    return m_sharedHandles;
}

//-----------------------------------------------------------------------------
// async does things differently
// release shared, placed resources and replace with render device resources.
//-----------------------------------------------------------------------------
void Compute::SetAsync(
    ComPtr<ID3D12Fence> in_fence,
    ComPtr<ID3D12Resource>* in_buffers,
    UINT in_bufferIndex)
{
    m_sharedRenderFence = in_fence;
    m_bufferIndex = 1-in_bufferIndex;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = m_numParticles;
    uavDesc.Buffer.StructureByteStride = sizeof(Render::Particle);
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        // replace "my" shared resources with the resources from the render adapter
        m_positionBuffers[i] = in_buffers[i];

        const CD3DX12_CPU_DESCRIPTOR_HANDLE heapHandle(
            m_srvHeap->GetCPUDescriptorHandleForHeapStart(),
            UavParticlePos0 + i,
            m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_positionBuffers[i].Get(), nullptr, &uavDesc, heapHandle);
    }
    // setting heap[2] = heap[0]
    const CD3DX12_CPU_DESCRIPTOR_HANDLE copyPosHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), UavParticlePos0Copy, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(m_positionBuffers[0].Get(), nullptr, &uavDesc, copyPosHandle);
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
    ThrowIfFailed(m_commandQueue->Wait(m_sharedRenderFence.Get(), in_sharedFenceValue-1));

    const UINT oldIndex = m_bufferIndex; // 0 or 1. Old corresponds to the surface the render device is currently using
    const UINT newIndex = 1 - oldIndex;  // 1 or 0. New corresponds to the surface the render device is NOT using

    ThrowIfFailed(m_commandAllocators[m_bufferIndex]->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_bufferIndex].Get(), m_computeState.Get()));

    m_pTimer->BeginTimer(m_commandList.Get(), static_cast<std::uint32_t>(GpuTimers::Simulate));

    const UINT srcHeapIndex = UavParticlePos0 + oldIndex; // 0 or 1

    ID3D12DescriptorHeap* ppHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    m_commandList->SetPipelineState(m_computeState.Get());
    m_commandList->SetComputeRootSignature(m_rootSignature.Get());
    m_commandList->SetComputeRootConstantBufferView(ComputeRootCBV, m_constantBufferCS->GetGPUVirtualAddress());

    //-------------------------------------------------
    // set heap base to point at previous simulation results
    // note that descriptor heap[2] is a copy of heap[0], so when the base is heap[1] the dest is heap[2]==heap[0]
    //-------------------------------------------------
    const CD3DX12_GPU_DESCRIPTOR_HANDLE srcHeapHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), srcHeapIndex, m_srvUavDescriptorSize);
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
