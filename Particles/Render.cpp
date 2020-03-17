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
#include <D3Dcompiler.h>
#include <sstream>

#include "Render.h"
#include "Particles.h"
#include "D3D12GpuTimer.h"
#include "ExtensionHelper.h" // Intel extensions

using namespace DirectX;
using Microsoft::WRL::ComPtr;

#define USE_LATENCY_WAITABLE 1

/*
NOTE on "common" state transition while using multi-engine
Resources must transition to/from the COMMON state before/after the copy engine, BUT
resources implicitly return to Common state after ExecuteCommandLists.

Hence, there's no reason to explicitly do barrier transitions to/from COMMON in this sample because
1. the resources are not used simultaneously
2. each queue waits on a fence from the other queue
3. ExecuteCommandLists() occurs relative to a fence such that resources implicitly decay to or promote from the COMMON state
*/

/*
https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states?redirectedfrom=MSDN
    Specifically, a resource must be in the COMMON state before being used on a COPY queue
    (when previous used on DIRECT/COMPUTE),
    and before being used on DIRECT/COMPUTE (when previously used on COPY).

    This restriction does not exist when accessing data between DIRECT and COMPUTE queues.

    The COMMON state can be used for all usages on a Copy queue using the implicit state transitions.

https://docs.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
    To use a resource initially on a Copy queue it should start in the COMMON state.
    The COMMON state can be used for all usages on a Copy queue using the implicit state transitions.

    Although resource state is shared across all Compute and 3D queues, it is not permitted to write
    to the resource simultaneously on different queues.

    "Simultaneously" here means unsynchronized, noting unsynchronized execution is not possible on some hardware.
    The following rules apply.
        * Only one queue can write to a resource at a time.
        * Multiple queues can read from the resource as long as they don’t read the bytes being modified by the writer (reading bytes being simultaneously written produces undefined results).
        * A fence must be used to synchronize after writing before another queue can read the written bytes or make any write access.

https://docs.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12#implicit-state-transitions
    Note that common state promotion is "free" in that there is no need for the GPU to perform any synchronization waits.

    The flip-side of common state promotion is decay back to D3D12_RESOURCE_STATE_COMMON.

    Resources that meet certain requirements are considered to be stateless and effectively return to the common state
    when the GPU finishes execution of an ExecuteCommandLists operation.

    The following resources will decay when an ExecuteCommandLists operation is completed on the GPU:
        * Resources being accessed on a Copy queue, or
        * Buffer resources on any queue type, or
        * Texture resources on any queue type that have the D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS flag set, or
        * Any resource implicitly promoted to a read-only state.
 */

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
bool Render::GetSupportsIntelCommandQueueExtension() const
{
    return m_pExtensionHelper->GetEnabled();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
std::wstring GetAssetFullPath(const std::wstring in_filename)
{
    constexpr size_t PATHBUFFERSIZE = MAX_PATH * 4;
    TCHAR buffer[PATHBUFFERSIZE];
    GetCurrentDirectory(_countof(buffer), buffer);
    std::wstring directory = buffer;
    return directory + L"\\\\" + in_filename;
}

// Indices of shader resources in the descriptor heap.
enum DescriptorHeapIndex : UINT32
{
    SrvParticlePosVelo0 = 0,
    SrvParticlePosVelo1,
    DescriptorCount
};

enum class GpuTimers
{
    FPS,
    NumTimers
};

//-----------------------------------------------------------------------------
// creates a command queue with the intel extension if available
//-----------------------------------------------------------------------------
void Render::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    m_commandQueue.Reset();
    m_copyQueue.Reset();

    if (m_usingIntelCommandQueueExtension)
    {
        m_commandQueue = m_pExtensionHelper->CreateCommandQueue(desc);
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        m_copyQueue = m_pExtensionHelper->CreateCommandQueue(desc);
    }
    else
    {
        ThrowIfFailed(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue)));
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ThrowIfFailed(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_copyQueue)));
    }
    m_commandQueue->SetName(L"Render Queue");
    m_copyQueue->SetName(L"Copy Queue");

    CreateSwapChain();
}

//-----------------------------------------------------------------------------
// Creates a command queue optionally using the intel throttle extension
// returns state after attempting set()
//-----------------------------------------------------------------------------
void Render::SetUseIntelCommandQueueExtension(bool in_desiredSetting)
{
    in_desiredSetting = in_desiredSetting && m_pExtensionHelper->GetEnabled();
    if (m_usingIntelCommandQueueExtension != in_desiredSetting)
    {
        m_usingIntelCommandQueueExtension = in_desiredSetting;
        CreateCommandQueue();
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Render::Render(HWND in_hwnd, UINT in_numParticles,
    Microsoft::WRL::ComPtr<IDXGIAdapter1> in_adapter,
    bool in_useIntelCommandQueueExtension,
    bool in_fullScreen, RECT in_windowDim) :
    m_numParticles(in_numParticles),
    m_frameFenceValues{}
{
    m_hwnd = in_hwnd;
    m_pConstantBufferGSData = 0;
    m_camera.Init({ 0.0f, 0.0f, 1500.0f });
    m_camera.SetMoveSpeed(250.0f);
    m_currentBufferIndex = 0;
    m_adapter = in_adapter;
    m_fullScreen = in_fullScreen;
    m_windowDim = in_windowDim;
    m_renderFenceValue = 0;
    m_windowedSupportsTearing = false;
    m_particleSize = 0;
    m_particleIntensity = 0;

    CreateDevice(in_adapter.Get(), m_device);

    // attempt to enable Intel extensions
    m_pExtensionHelper = new ExtensionHelper(m_device.Get());
    m_usingIntelCommandQueueExtension = in_useIntelCommandQueueExtension && m_pExtensionHelper->GetEnabled();
    CreateCommandQueue();

    LoadAssets();

    m_pTimer = new D3D12GpuTimer(m_device.Get(), m_commandQueue.Get(), static_cast<UINT>(GpuTimers::NumTimers));
    m_pTimer->SetTimerName(static_cast<UINT>(GpuTimers::FPS), "render ms");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Render::~Render()
{
    WaitForGpu();

    if (m_fullScreen)
    {
        // be sure to leave things in windowed state
        m_swapChain->SetFullscreenState(FALSE, nullptr);
    }
    if (m_pConstantBufferGSData)
    {
        CD3DX12_RANGE readRange(0, 0);
        m_constantBufferGS->Unmap(0, &readRange);
        m_pConstantBufferGSData = 0;
    }

    CloseHandle(m_sharedFenceHandle);

    delete m_pExtensionHelper;
}

//-----------------------------------------------------------------------------
// get handles to textures the simulation results will be copied to
//-----------------------------------------------------------------------------
void Render::SetShared(Compute::SharedHandles in_sharedHandles)
{
    m_sharedBufferIndex = in_sharedHandles.m_bufferIndex;

    ID3D12Heap* pSharedHeap = 0;
    m_device->OpenSharedHandle(in_sharedHandles.m_heap, IID_PPV_ARGS(&pSharedHeap));

    m_device->OpenSharedHandle(in_sharedHandles.m_fence, IID_PPV_ARGS(&m_sharedComputeFence));

    D3D12_RESOURCE_DESC crossAdapterDesc = CD3DX12_RESOURCE_DESC::Buffer(in_sharedHandles.m_alignedDataSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        ThrowIfFailed(m_device->CreatePlacedResource(
            pSharedHeap,
            i * in_sharedHandles.m_alignedDataSize,
            &crossAdapterDesc,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            nullptr,
            IID_PPV_ARGS(&m_sharedBuffers[i])));
#ifdef _DEBUG
        std::wostringstream wss;
        wss << "Local-" << i;
        m_buffers[i]->SetName(wss.str().c_str());
#endif
    }
    pSharedHeap->Release();

    // copy initial state from the other adapter
    // NOTE: this copy was moved from the copy queue to the direct queue to avoid an (erroneous?) debug layer warning/error on the dest resource
    // this could just as easily be done on the copy command queue,
    // BUT the copy command queue can't do the transition the debug layer requests
    {
        auto pDstResource = m_buffers;
        auto pSrcResource = m_sharedBuffers;

        m_commandAllocators[m_frameIndex]->Reset();
        ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr));

        for (int i = 0; i < m_NUM_BUFFERS; i++)
        {
            m_commandList->CopyBufferRegion(pDstResource[i].Get(), 0, pSrcResource[i].Get(), 0, m_bufferSize);
        }

        ThrowIfFailed(m_commandList->Close());
        ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, ppCommandLists);

        WaitForGpu();
    }
}

//-----------------------------------------------------------------------------
// note creating the swap chain requires a command queue
// hence, if the command queue changes, we must re-create the swap chain
// command queue can change if we toggle the Intel command queue extension
//-----------------------------------------------------------------------------
void Render::CreateSwapChain()
{
    ComPtr<IDXGIFactory5> factory = nullptr;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory))))
    {
        flags &= ~DXGI_CREATE_FACTORY_DEBUG;
        ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)));
    }

    // tearing supported for full-screen borderless windows?
    if (m_fullScreen)
    {
        m_windowedSupportsTearing = false;
    }
    else
    {
        BOOL allowTearing = FALSE;
        factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
        if (allowTearing)
        {
            m_windowedSupportsTearing = true;
        }
    }

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = NUM_FRAMES;
    swapChainDesc.Width = m_windowDim.right - m_windowDim.left;
    swapChainDesc.Height = m_windowDim.bottom - m_windowDim.top;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = 0;
#if USE_LATENCY_WAITABLE
    swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#endif
    if (m_windowedSupportsTearing)
    {
        swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullScreenDesc = nullptr;

    // if full screen mode, launch into the current settings
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullScreenDesc = {};
    IDXGIOutput* pOutput = nullptr;

    // on switch to full screen, try to move to a monitor attached to the adapter
    // if no monitor attached, just use the "current" display
    if (m_fullScreen)
    {
        // get the dimensions of the primary monitor, same as GetDeviceCaps( hdcPrimaryMonitor, HORZRES)
        swapChainDesc.Width = GetSystemMetrics(SM_CXSCREEN);
        swapChainDesc.Height = GetSystemMetrics(SM_CYSCREEN);
        // primary monitor has 0,0 as top-left
        UINT left = 0;
        UINT top = 0;

        HRESULT foundOutput = 0;
        // take the first attached monitor
        m_adapter->EnumOutputs(0, &pOutput);
        if (pOutput)
        {
            DXGI_OUTPUT_DESC outputDesc;
            pOutput->GetDesc(&outputDesc);
            swapChainDesc.Width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
            swapChainDesc.Height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
            left = outputDesc.DesktopCoordinates.left;
            top = outputDesc.DesktopCoordinates.top;
        }
        SetWindowLongPtr(m_hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
        SetWindowPos(m_hwnd, HWND_TOP, left, top, swapChainDesc.Width, swapChainDesc.Height,
            SWP_FRAMECHANGED);

        fullScreenDesc.Windowed = FALSE;
        pFullScreenDesc = &fullScreenDesc;
    }

    ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = factory->CreateSwapChainForHwnd(m_commandQueue.Get(), m_hwnd,
        &swapChainDesc, pFullScreenDesc, pOutput, &swapChain);

    m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f,
        static_cast<FLOAT>(swapChainDesc.Width), static_cast<FLOAT>(swapChainDesc.Height));
    m_scissorRect = CD3DX12_RECT(0, 0, swapChainDesc.Width, swapChainDesc.Height);

    /*
    want full screen with tearing.
    from MSDN, DXGI_PRESENT_ALLOW_TEARING:
    - The swap chain must be created with the DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING flag.
    - It can only be used in windowed mode.
    - To use this flag in full screen Win32 apps, the application should present to a fullscreen borderless window
    and disable automatic ALT+ENTER fullscreen switching using IDXGIFactory::MakeWindowAssociation.
     */
    ThrowIfFailed(factory->MakeWindowAssociation(m_hwnd,
        DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_PRINT_SCREEN));

    ThrowIfFailed(swapChain.As(&m_swapChain));

    if (m_fullScreen)
    {
        m_swapChain->SetFullscreenState(TRUE, nullptr);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
#if USE_LATENCY_WAITABLE
    m_swapChainEvent = m_swapChain->GetFrameLatencyWaitableObject();

    // from MSDN:
    // Note that it is important to call this before the first Present
    // in order to minimize the latency of the swap chain.
    WaitForSingleObjectEx(m_swapChainEvent, 1000, TRUE);
#else
    m_swapChainEvent = 0;
#endif

    m_aspectRatio = static_cast<float>(swapChainDesc.Width) / static_cast<float>(swapChainDesc.Height);
}

//-----------------------------------------------------------------------------
// 1 texture, the particle
// 1 rendertarget, 2 frames
// no depth buffer
//-----------------------------------------------------------------------------
void Render::LoadAssets()
{
    // Describe and create a shader resource view (SRV) descriptor heap.
    D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
    srvUavHeapDesc.NumDescriptors = DescriptorCount;
    srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&m_srvHeap)));
    NAME_D3D12_OBJECT(m_srvHeap);

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    //-------------------------------------------------------------------------
    // Create root signature
    //-------------------------------------------------------------------------
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

        // This is the highest version the sample supports. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

        CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

        CD3DX12_ROOT_PARAMETER1 rootParameters[GraphicsRootParametersCount];
        rootParameters[GraphicsRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[GraphicsRootSRVTable].InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_VERTEX);

        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = NUM_FRAMES;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
    }

    //-------------------------------------------------------------------------
    // Create frame resources
    //-------------------------------------------------------------------------
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV and a command allocator for each frame.
        for (std::uint32_t i = 0; i < NUM_FRAMES; i++)
        {
            (m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
            m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);

            ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])));
            ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_copyAllocators[i])));

            std::wstringstream cmdAllocName;
            cmdAllocName << "Render CmdAlloc " << i;
            m_commandAllocators[i]->SetName(cmdAllocName.str().c_str());

            std::wstringstream copyAllocName;
            copyAllocName << "Copy CmdAlloc " << i;
            m_copyAllocators[i]->SetName(copyAllocName.str().c_str());
        }
    }

    //-------------------------------------------------------------------------
    // Create the pipeline states, which includes compiling and loading shaders
    //-------------------------------------------------------------------------
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> geometryShader;
        ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        // Load and compile shaders.
        ID3DBlob* pErrorMsgs = 0;
        const wchar_t* pShaderName = L"ParticleDraw.hlsl";

        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(pShaderName).c_str(), nullptr, nullptr, "VSParticleDraw", "vs_5_0", compileFlags, 0, &vertexShader, &pErrorMsgs));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(pShaderName).c_str(), nullptr, nullptr, "GSParticleDraw", "gs_5_0", compileFlags, 0, &geometryShader, &pErrorMsgs));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(pShaderName).c_str(), nullptr, nullptr, "PSParticleDraw", "ps_5_0", compileFlags, 0, &pixelShader, &pErrorMsgs));

        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
        {
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // Describe the blend and depth states.
        CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

        CD3DX12_DEPTH_STENCIL_DESC depthStencilDesc(D3D12_DEFAULT);
        depthStencilDesc.DepthEnable = FALSE;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.GS = CD3DX12_SHADER_BYTECODE(geometryShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = depthStencilDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));

        ThrowIfFailed(hr);
        NAME_D3D12_OBJECT(m_pipelineState);
    }

    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    m_commandList->SetName(L"Render CommandList");

    // command list is used by the following methods:
    CreateVertexBuffer();
    CreateParticleBuffers();

    //-------------------------------------------------------------------------
    // Create the geometry shader's constant buffer.
    //-------------------------------------------------------------------------
    {
        const UINT constantBufferGSSize = sizeof(ConstantBufferGS) * NUM_FRAMES;

        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(constantBufferGSSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferGS)
        ));

        NAME_D3D12_OBJECT(m_constantBufferGS);

        CD3DX12_RANGE readRange(0, 0);        // We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_constantBufferGS->Map(0, &readRange, reinterpret_cast<void**>(&m_pConstantBufferGSData)));
        ZeroMemory(m_pConstantBufferGSData, constantBufferGSSize);
    }

    // init resources, e.g. upload initial partical positions
    m_commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        ThrowIfFailed(m_device->CreateFence(
            m_renderFenceValue,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_renderFence)));
        m_renderFenceValue++;

        // Create an event handle to use for frame synchronization.
        m_renderFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_renderFenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    // copy fence is shared
    {
        ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_copyAllocators[m_frameIndex].Get(), nullptr, IID_PPV_ARGS(&m_copyList)));
        m_copyList->SetName(L"Copy CommandList");
        m_copyList->Close();

        m_copyFenceValue = 0;
        ThrowIfFailed(m_device->CreateFence(
            m_copyFenceValue,
            D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
            IID_PPV_ARGS(&m_copyFence)));

        ThrowIfFailed(m_device->CreateSharedHandle(m_copyFence.Get(), nullptr, GENERIC_ALL, L"RenderSharedFence", &m_sharedFenceHandle));
    }

    // Wait for the command list to execute; we are reusing the same command 
    // list in our main loop but for now, we just want to wait for setup to 
    // complete before continuing.
    WaitForGpu();
}

//-----------------------------------------------------------------------------
// Wait for pending GPU work to complete.
// not interacting with swap chain.
//-----------------------------------------------------------------------------
void Render::WaitForGpu()
{
    // wait for copy queue too by signaling it and having the render command queue wait on the fence
    m_copyFenceValue++;
    ThrowIfFailed(m_copyQueue->Signal(m_copyFence.Get(), m_copyFenceValue));
    ThrowIfFailed(m_commandQueue->Wait(m_copyFence.Get(), m_copyFenceValue));

    // Add a signal command to the queue.
    ThrowIfFailed(m_commandQueue->Signal(m_renderFence.Get(), m_renderFenceValue));

    // Instruct the fence to set the event object when the signal command completes.
    ThrowIfFailed(m_renderFence->SetEventOnCompletion(m_renderFenceValue, m_renderFenceEvent));
    m_renderFenceValue++;

    // Wait until the signal command has been processed.
    WaitForSingleObject(m_renderFenceEvent, INFINITE);
}

//-----------------------------------------------------------------------------
// modified next-frame logic to return a handle if a wait is required.
// NOTE: be sure to check for non-null handle before WaitForSingleObjectEx() (or equivalent)
//-----------------------------------------------------------------------------
HANDLE Render::MoveToNextFrame()
{
    // Assign the current fence value to the current frame.
    m_frameFenceValues[m_frameIndex] = m_renderFenceValue;

    // Signal and increment the fence value.
    ThrowIfFailed(m_commandQueue->Signal(m_renderFence.Get(), m_renderFenceValue));
    m_renderFenceValue++;

    // Update the frame index.
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is ready.
    HANDLE returnHandle = 0;
    if (m_renderFence->GetCompletedValue() < m_frameFenceValues[m_frameIndex])
    {
        ThrowIfFailed(m_renderFence->SetEventOnCompletion(m_frameFenceValues[m_frameIndex], m_renderFenceEvent));
        //WaitForSingleObject(m_renderFenceEvent, INFINITE);
        returnHandle = m_renderFenceEvent;
    }

    return returnHandle;
}

//-----------------------------------------------------------------------------
// Create the particle vertex buffer.
//-----------------------------------------------------------------------------
void Render::CreateVertexBuffer()
{
    std::vector<ParticleVertex> vertices;
    vertices.resize(m_numParticles);
    for (UINT i = 0; i < m_numParticles; i++)
    {
        vertices[i].color = XMFLOAT4(1.0f, 1.0f, 0.2f, 1.0f);
    }
    const UINT bufferSize = m_numParticles * sizeof(ParticleVertex);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_vertexBuffer)));

    ThrowIfFailed(m_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_vertexBufferUpload)));

    NAME_D3D12_OBJECT(m_vertexBuffer);

    D3D12_SUBRESOURCE_DATA vertexData = {};
    vertexData.pData = reinterpret_cast<UINT8*>(&vertices[0]);
    vertexData.RowPitch = bufferSize;
    vertexData.SlicePitch = vertexData.RowPitch;

    UpdateSubresources<1>(m_commandList.Get(), m_vertexBuffer.Get(), m_vertexBufferUpload.Get(), 0, 0, 1, &vertexData);
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(bufferSize);
    m_vertexBufferView.StrideInBytes = sizeof(ParticleVertex);
}

//-----------------------------------------------------------------------------
// Create the position and velocity buffer shader resources.
//-----------------------------------------------------------------------------
void Render::CreateParticleBuffers()
{
    m_bufferSize = m_numParticles * sizeof(Particle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = m_numParticles;
    srvDesc.Buffer.StructureByteStride = sizeof(Particle);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    for (UINT i = 0; i < m_NUM_BUFFERS; i++)
    {
        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(m_bufferSize, D3D12_RESOURCE_FLAG_NONE),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_buffers[i])));
#ifdef _DEBUG
        std::wostringstream wss;
        wss << "Shared-" << i;
        m_buffers[i]->SetName(wss.str().c_str());
#endif

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart(), SrvParticlePosVelo0 + i, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_buffers[i].Get(), &srvDesc, srvHandle);
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Render::UpdateCamera()
{
#if USE_LATENCY_WAITABLE
    // Wait for the previous Present to complete.
    WaitForSingleObjectEx(m_swapChainEvent, 1000, FALSE);
#endif
    //m_timer.Tick(NULL);
    //m_camera.Update(static_cast<float>(m_timer.GetElapsedSeconds()));
    m_camera.Update(0);

    ConstantBufferGS constantBufferGS = {};
    XMStoreFloat4x4(&constantBufferGS.worldViewProjection, XMMatrixMultiply(m_camera.GetViewMatrix(), m_camera.GetProjectionMatrix(0.8f, m_aspectRatio, 1.0f, 5000.0f)));
    XMStoreFloat4x4(&constantBufferGS.inverseView, XMMatrixInverse(nullptr, m_camera.GetViewMatrix()));
    constantBufferGS.particleSize = m_particleSize;
    constantBufferGS.particleIntensity = m_particleIntensity;

    UINT8* destination = m_pConstantBufferGSData + sizeof(ConstantBufferGS) * m_frameIndex;
    memcpy(destination, &constantBufferGS, sizeof(ConstantBufferGS));
}

//-------------------------------------------------
// copy simulation results from compute adapter
//-------------------------------------------------
void Render::CopySimulationResults(UINT64 in_fenceValue, int in_numActiveParticles)
{
    //-------------------------------------------------------------------------
    // multi-engine sync
    // wait on previous frame to finish
    // race: can't do a wait on current frame (m_renderFenceValue) after the copy.
    //-------------------------------------------------------------------------
    ThrowIfFailed(m_copyQueue->Wait(m_renderFence.Get(), m_renderFenceValue-1));

    UINT srcSharedIndex = 1 - m_sharedBufferIndex;     // reading from shared buffer pointed to by m_sharedBufferIndex
    UINT dstLocalIndex = 1 - m_currentBufferIndex; // writing to local buffer pointed to by m_currentBufferIndex
    m_sharedBufferIndex = 1 - m_sharedBufferIndex; // move shared index forward for next time

    ID3D12Resource* pDstResource = m_buffers[dstLocalIndex].Get();
    ID3D12Resource* pSrcResource = m_sharedBuffers[srcSharedIndex].Get();

    m_copyAllocators[m_frameIndex]->Reset();
    ThrowIfFailed(m_copyList->Reset(m_copyAllocators[m_frameIndex].Get(), nullptr));

    // a resource barrier gives maximum information to the runtime that may help other adapters with cache sync
    // it should not be necessary on a copy queue, especially when using buffers
    m_copyList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(pSrcResource));

    // the aligned data size of the shared buffer could be larger than the buffer contents
    // copy just the particles required
    m_copyList->CopyBufferRegion(pDstResource, 0, pSrcResource, 0, in_numActiveParticles * sizeof(Particle));

    ThrowIfFailed(m_copyList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_copyList.Get() };
    m_copyQueue->ExecuteCommandLists(1, ppCommandLists);

    //-------------------------------------------------------------------------
    // multi-engine sync
    // don't start next copy until the compute gpu has produced new results
    // also helps host-side sync, since host will wait on render fence, render waits on copy, and copy waits on compute
    //-------------------------------------------------------------------------
    ThrowIfFailed(m_copyQueue->Wait(m_sharedComputeFence.Get(), in_fenceValue));

    // signal the copy fence
    m_copyFenceValue++;
    ThrowIfFailed(m_copyQueue->Signal(m_copyFence.Get(), m_copyFenceValue));
}

//-----------------------------------------------------------------------------
// Draw() tells Particles to draw its UI
// input is compute fence value. output is render fence value.
// normally, in_numParticlesCopied should equal in_numActiveParticles
// in_numParticlesCopied was added to experiment with stressing the PCI bus
//-----------------------------------------------------------------------------
HANDLE Render::Draw(int in_numActiveParticles, Particles* in_pParticles, UINT64& inout_fenceValue,
    int in_numParticlesCopied)
{
    UpdateCamera();

    // start copy for next frame. no reason to delay.
    CopySimulationResults(inout_fenceValue, in_numParticlesCopied);

    m_commandAllocators[m_frameIndex]->Reset();
    ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get()));

    m_pTimer->BeginTimer(m_commandList.Get(), static_cast<std::uint32_t>(GpuTimers::FPS));

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->SetGraphicsRootConstantBufferView(GraphicsRootCBV, m_constantBufferGS->GetGPUVirtualAddress() + m_frameIndex * sizeof(ConstantBufferGS));

    // srvheap holds particle velocities in SRV form
    ID3D12DescriptorHeap* ppHeaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    // current buffer index refers to the physically local buffer holding particle positions
    // use the current buffer index, then update the current buffer index for the next frame
    const UINT srvIndex = SrvParticlePosVelo0 + m_currentBufferIndex;
    ID3D12Resource* pResource = m_buffers[m_currentBufferIndex].Get();
    m_currentBufferIndex = 1 - m_currentBufferIndex;

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), srvIndex, m_srvUavDescriptorSize);
    m_commandList->SetGraphicsRootDescriptorTable(GraphicsRootSRVTable, srvHandle);

    // draw things
    {
        // transition pixel position from/to copy dest
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pResource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        m_commandList->DrawInstanced(in_numActiveParticles, 1, 0, 0);
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));

        // Draw GUI last
#if IMGUI_ENABLED
        in_pParticles->DrawGUI(m_commandList.Get());
#endif

        // Indicate that the back buffer will now be used to present.
        m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
    }

    m_pTimer->EndTimer(m_commandList.Get(), static_cast<std::uint32_t>(GpuTimers::FPS));
    m_pTimer->ResolveAllTimers(m_commandList.Get());

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    //-------------------------------------------------------------------------
    // Present the frame.
    //-------------------------------------------------------------------------
    UINT syncInterval = in_pParticles->GetVsyncEnabled() ? 1 : 0;
    UINT presentFlags = 0;
    if ((m_windowedSupportsTearing) && (!m_fullScreen) && (0 == syncInterval))
    {
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
    }
    ThrowIfFailed(m_swapChain->Present(syncInterval, presentFlags));

    //-------------------------------------------------------------------------
    // multi-engine and multi-adapter sync
    // for host-side sync, we return a handle to the whole multi-adapter pipeline
    // this wait(), by virtue of copy sync with compute, this also syncs render and compute
    //-------------------------------------------------------------------------
    ThrowIfFailed(m_commandQueue->Wait(m_copyFence.Get(), m_copyFenceValue));

    //-------------------------------------------------------------------------
    // end of frame
    //-------------------------------------------------------------------------
    inout_fenceValue = m_copyFenceValue;
    return MoveToNextFrame();
}
