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
#include <algorithm> // for std::min()

#include <d3d12.h>
#include <dxgidebug.h>
#include <shellapi.h> // for CommandLineToArgvW

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include "Particles.h"
#include "Compute.h"
#include "Render.h"
#include "ArgParser.h"

//-----------------------------------------------------------------------------
// discover adapters
// save info so roles can be dynamically changed
//-----------------------------------------------------------------------------
Particles::Particles(HWND in_hwnd)
    : m_hwnd(in_hwnd)

    , m_pRender(nullptr)
    , m_pCompute(nullptr)

    , m_renderAdapterIndex(0)
    , m_computeAdapterIndex(0)

    , m_commandQueueExtensionEnabled(false)
    , m_vsyncEnabled(true)
    , m_fullScreen(false)

    , m_height(0)

    , m_particleSize(INITIAL_PARTICLE_SIZE)
    , m_particleIntensity(INITIAL_PARTICLE_INTENSITY)

    , m_maxNumParticles(MAX_NUM_PARTICLES)
    , m_numParticlesRendered(MAX_NUM_PARTICLES)
    , m_numParticlesCopied(MAX_NUM_PARTICLES)
    , m_numParticlesSimulated(MAX_NUM_PARTICLES)
    , m_numParticlesLinked(true)

    , m_enableUI(true)
    , m_enableExtensions(true)
{
    ParseCommandLine();

    m_windowInfo.cbSize = sizeof(WINDOWINFO);
    const BOOL rv = ::GetWindowInfo(m_hwnd, &m_windowInfo);
    assert(rv);

    // Enable the D3D12 debug layer
    {
        ComPtr<ID3D12Debug1> pDebugController;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
        {
            //pDebugController->SetEnableGPUBasedValidation(TRUE);
            pDebugController->EnableDebugLayer();
        }
    }

    UINT flags = 0;
#ifdef _DEBUG
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    if (FAILED(::CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_dxgiFactory))))
    {
        flags &= ~DXGI_CREATE_FACTORY_DEBUG;
        ThrowIfFailed(::CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_dxgiFactory)));
    }

    // find adapters
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        ThrowIfFailed(adapter->GetDesc1(&desc));

        if (((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) && (desc.VendorId != 5140))
        {
            m_adapters.push_back(adapter);

            std::string narrowString;
            const int numChars = ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, _countof(desc.Description), nullptr, 0, nullptr, nullptr);
            narrowString.resize(numChars);
            ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, (int)narrowString.size(), &narrowString[0], numChars, nullptr, nullptr);

            // m_adapterDescriptions holds the strings
            m_adapterDescriptions.push_back(narrowString);
            // m_adapterDescriptionPtrs is an array of pointers into m_adapterDescriptions, and is used by imgui
            m_adapterDescriptionPtrs.push_back(m_adapterDescriptions.back().c_str());
        }
    }

    // initial state
    const size_t numAdapters = m_adapters.size();
    if (numAdapters > 0)
    {
        AssignAdapters();

        m_pRender = new Render(m_hwnd, m_maxNumParticles, m_adapters[m_renderAdapterIndex].Get(), m_commandQueueExtensionEnabled, m_fullScreen, m_windowInfo.rcClient);
        m_pCompute = new Compute(m_maxNumParticles, m_adapters[m_computeAdapterIndex].Get(), m_commandQueueExtensionEnabled);

        ShareHandles();

        m_commandQueueExtensionEnabled = m_pCompute->GetUsingIntelCommandQueueExtension() ||
            m_pRender->GetUsingIntelCommandQueueExtension();
    }
    else
    {
        throw;
    }

    if (m_enableUI)
    {
        //-----------------------------
        // one-time UI setup
        //-----------------------------
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO(); (void)io;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableSetMousePos + ImGuiBackendFlags_HasSetMousePos;  // Enable Keyboard Controls

            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(m_hwnd);
        }

        // render device specific setup
        InitGui();
    }

    // initial state for UI toggles
    m_prevRenderAdapterIndex = m_renderAdapterIndex;
    m_prevComputeAdapterIndex = m_computeAdapterIndex;
    m_prevQueueExtension = m_commandQueueExtensionEnabled;
    m_prevFullScreen = m_fullScreen;

    // start frame duration timer
    m_frameTimer.Start();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Particles::~Particles()
{
    delete m_pCompute;
    delete m_pRender;

    if (m_enableUI)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(nullptr);
    }
}

//-----------------------------------------------------------------------------
// share handles between render and compute
// optionally, copy particle state from render to compute (usually compute creates particle state)
//-----------------------------------------------------------------------------
void Particles::ShareHandles()
{
    assert(m_pRender != nullptr);
    assert(m_pCompute != nullptr);

    m_pCompute->ResetFromAsyncHelper();

    const HANDLE renderFenceHandle = m_pRender->GetSharedFenceHandle();
    assert(renderFenceHandle != nullptr);
    m_pRender->SetShared(m_pCompute->GetSharedHandles(renderFenceHandle));

    bool asyncMode = (m_renderAdapterIndex == m_computeAdapterIndex);
    if (asyncMode)
    {
        m_pCompute->SetAsync(m_pRender->GetFence(), m_pRender->GetBuffers(), m_pRender->GetBufferIndex());
    }
    m_pRender->SetAsyncMode(asyncMode);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Particles::AssignAdapters()
{
    const size_t numAdapters = m_adapters.size();

    // if no UMA (integrated) device is found, then:
    // compute will be the first adapter enumerated (index 0)
    // render will be the last adapter enumerated (# adapters - 1)
    for (size_t i = 0; i < numAdapters; i++)
    {
        ComPtr<ID3D12Device> device;
        ThrowIfFailed(::D3D12CreateDevice(m_adapters[i].Get(), MINIMUM_D3D_FEATURE_LEVEL, IID_PPV_ARGS(&device)));

        // check for UMA support (uses system memory as local memory)
        D3D12_FEATURE_DATA_ARCHITECTURE featureData = {};
        const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &featureData, sizeof(featureData));
        if (SUCCEEDED(hr) && featureData.UMA)
        {
            m_computeAdapterIndex = int(i);
        }
        else
        {
            m_renderAdapterIndex = int(i);
        }
    }

    // in the case where all devices are the same type (or only 1 device):
    if (m_computeAdapterIndex == m_renderAdapterIndex)
    {
        m_computeAdapterIndex = 0;
        m_renderAdapterIndex = int(numAdapters - 1);
    }
}

//-----------------------------------------------------------------------------
// parse command line
//-----------------------------------------------------------------------------
void Particles::ParseCommandLine()
{
    ArgParser argParser;
    argParser.AddArg(L"numparticles", [=](std::wstring s) {
        m_maxNumParticles = std::stoi(s);
        m_numParticlesRendered = m_maxNumParticles;
        m_numParticlesCopied = m_maxNumParticles;
        m_numParticlesSimulated = m_maxNumParticles;
    });

    argParser.AddArg(L"nogui", m_enableUI);
    argParser.AddArg(L"noext", m_enableExtensions);
    argParser.AddArg(L"size", m_particleSize);
    argParser.AddArg(L"intensity", m_particleIntensity);
    argParser.AddArg(L"novsync", m_vsyncEnabled);
    argParser.AddArg(L"fullscreen", m_fullScreen);

    argParser.AddArg(L"numCopy", [=](std::wstring s) { m_numParticlesCopied = std::stoi(s); m_numParticlesLinked = false; });
    argParser.AddArg(L"numDraw", [=](std::wstring s) { m_numParticlesRendered = std::stoi(s); m_numParticlesLinked = false; });
    argParser.AddArg(L"numSim", [=](std::wstring s) { m_numParticlesSimulated = std::stoi(s); m_numParticlesLinked = false; });

    argParser.Parse();
}

//-----------------------------------------------------------------------------
// Initialize UI resources
// create SRV heap and root signature
// device comes from the Render object
//-----------------------------------------------------------------------------
void Particles::InitGui()
{
    if (!m_enableUI)
    {
        return;
    }
    m_srvHeap.Reset();

    ID3D12Device* pDevice = m_pRender->GetDevice();

    // Describe and create a shader resource view (SRV) heap for the texture.
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 128; // FIXME: enough?
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(pDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpu(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_GPU_DESCRIPTOR_HANDLE gpu(m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplDX12_Init(
        pDevice,
        Render::GetNumFrames(),
        DXGI_FORMAT_R8G8B8A8_UNORM,
        cpu.Offset(0, pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)),
        gpu.Offset(0, pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    ImGui_ImplDX12_CreateDeviceObjects();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Particles::DrawGUI(ID3D12GraphicsCommandList* in_pCommandList)
{
    if (!m_enableUI)
    {
        return;
    }

    const float m_guiWidth = 300;
    const float m_guiHeight = (float)m_height;

    ID3D12DescriptorHeap* ppHeaps[] = { m_srvHeap.Get() };
    in_pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImVec2 v(0, 0);
    ImVec2 s(static_cast<float>(m_guiWidth), static_cast<float>(m_guiHeight));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);

    // Show a simple window that we create ourselves. We use a Begin/End pair to created a named window.
    ImGui::Begin("Test", 0,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar); // Create a window called "Hello, world!" and append into it.
    ImGui::SetWindowPos(v);
    ImGui::SetWindowSize(s);

    ImGui::TextUnformatted("Adapters");
    {
        ImGui::ListBox(
            "Render",
            &m_renderAdapterIndex,
            m_adapterDescriptionPtrs.data(),
            (int)m_adapterDescriptionPtrs.size());
    }

    {
        ImGui::ListBox(
            "Compute",
            &m_computeAdapterIndex,
            m_adapterDescriptionPtrs.data(),
            (int)m_adapterDescriptionPtrs.size());
    }

    if (m_renderAdapterIndex == m_computeAdapterIndex)
    {
        ImGui::Text("Single Adapter with Async Compute");
    }
    else
    {
        if (m_pCompute->GetIsUMA())
        {
            ImGui::Text("Good: Multi-GPU with UMA Compute");
        }
        else
        {
            ImGui::Text("PERFORMANCE ISSUE: Compute is not UMA");
        }
    }

    ImGui::Checkbox("Intel Q Extension", &m_commandQueueExtensionEnabled);
    ImGui::Checkbox("VSync", &m_vsyncEnabled);
    ImGui::Checkbox("FullScreen", &m_fullScreen);
    ImGui::SliderFloat("Size", &m_particleSize, 1, 10);
    ImGui::SliderFloat("Intensity", &m_particleIntensity, 0.1f, 2.0f);

    //-----------------------------------------------------
    // independently specify size of workload across engines/adapters
    //-----------------------------------------------------
    ImGui::Separator();
    ImGui::Text("Num Particles");

    int* numParticlesRendered = &m_numParticlesRendered;
    int* numParticlesCopied = &m_numParticlesCopied;
    int* numParticlesSimulated = &m_numParticlesSimulated;
    if (m_numParticlesLinked)
    {
        numParticlesCopied = numParticlesRendered;
        numParticlesSimulated = numParticlesRendered;
    }

    ImGui::SliderInt("Rendered", numParticlesRendered, std::min<int>(MIN_NUM_PARTICLES, m_maxNumParticles), m_maxNumParticles);
    ImGui::SliderInt("Copied", numParticlesCopied, std::min<int>(MIN_NUM_PARTICLES, m_maxNumParticles), m_maxNumParticles);
    ImGui::SliderInt("Simulated", numParticlesSimulated, std::min<int>(MIN_NUM_PARTICLES, m_maxNumParticles), m_maxNumParticles);
    ImGui::Checkbox("Link Sliders", &m_numParticlesLinked);

    //-----------------------------------------------------
    // timers
    //-----------------------------------------------------
    ImGui::Separator();

    for (auto& t : m_pRender->GetGpuTimes())
    {
        ImGui::Text("%s: %f", t.second.c_str(), t.first * 1000.0f);
    }
    for (auto& t : m_pCompute->GetGpuTimes())
    {
        ImGui::Text("%s: %f", t.second.c_str(), t.first * 1000.0f);
    }
    ImGui::Text("frameTime: %f", m_frameTimer.Get() * 1000.0f);
    //-----------------------------------------------------

    // resize the UI to fit the dynamically-sized components
    // first frame may be wrong, don't care.
    m_height = 10 + (uint32_t)ImGui::GetCursorPosY();

    ImGui::End();
    ImGui::PopStyleVar();

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), in_pCommandList);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Particles::Shutdown()
{

}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void Particles::Draw()
{
    m_frameTimer.Update();

    m_pRender->SetParticleSize(m_particleSize);
    m_pRender->SetParticleIntensity(m_particleIntensity);

    if (m_numParticlesLinked)
    {
        m_numParticlesCopied = m_numParticlesRendered;
        m_numParticlesSimulated = m_numParticlesRendered;
    }

    // start simulation. This also starts copy of results for next frame
    UINT64 renderSharedFenceValue = m_pCompute->GetFenceValue();
    const HANDLE drawHandle = m_pRender->Draw(m_numParticlesRendered, this, renderSharedFenceValue, m_numParticlesCopied);
    m_pCompute->Simulate(m_numParticlesSimulated, renderSharedFenceValue);

    // because the command lists of each adapter wait() on each other,
    // only need to host-wait() around the Present() on the render adapter
    if (drawHandle)
    {
        const DWORD rv = ::WaitForSingleObjectEx(drawHandle, INFINITE, FALSE);
        assert(rv == WAIT_OBJECT_0);
    }

    bool changeFullScreen = (m_prevFullScreen != m_fullScreen);
    bool changeQueueExtension = (m_prevQueueExtension != m_commandQueueExtensionEnabled);
    bool changeComputeDevice = (m_prevComputeAdapterIndex != m_computeAdapterIndex);
    bool changeRenderDevice = (m_prevRenderAdapterIndex != m_renderAdapterIndex)
        || (changeQueueExtension && m_pRender->GetSupportsIntelCommandQueueExtension())
        || changeFullScreen;

    // if anything changed that might result in an adapter being removed,
    // drain all the pipelines
    if (changeComputeDevice || changeRenderDevice)
    {
        m_pRender->WaitForGpu();
        m_pCompute->WaitForGpu();
    }

    //-----------------------------------------------------
    // Handle GUI changes
    //-----------------------------------------------------

    // switching from windowed to full screen? remember window state
    if (changeFullScreen && m_fullScreen)
    {
        assert(m_windowInfo.cbSize == sizeof(WINDOWINFO));
        const BOOL rv = ::GetWindowInfo(m_hwnd, &m_windowInfo);
        assert(rv);
    }

    // new render device?
    // this became more complicated because changing the render queue (by enabling extension) requires reset of swapchain
    // added some extra logic to check if the render doesn't support the extension, because reset of full-screen is annoying
    if (changeRenderDevice)
    {
        delete m_pRender;

        // for windowed mode, reset the window style and position before creating new Render
        if (changeFullScreen && !m_fullScreen)
        {
            const UINT width = m_windowInfo.rcWindow.right - m_windowInfo.rcWindow.left;
            const UINT height = m_windowInfo.rcWindow.bottom - m_windowInfo.rcWindow.top;
            const UINT left = m_windowInfo.rcWindow.left;
            const UINT top = m_windowInfo.rcWindow.top;

            ::SetWindowLongPtr(m_hwnd, GWL_STYLE, m_windowInfo.dwStyle);
            ::SetWindowPos(m_hwnd, HWND_NOTOPMOST, left, top, width, height, SWP_FRAMECHANGED);
        }

        m_pRender = new Render(m_hwnd, m_maxNumParticles, m_adapters[m_renderAdapterIndex].Get(), m_commandQueueExtensionEnabled, m_fullScreen, m_windowInfo.rcClient);

        InitGui();

        ShareHandles();
    }

    // new compute device?
    if (changeComputeDevice)
    {
        Compute* pOldCompute = m_pCompute;
        m_pCompute = new Compute(m_maxNumParticles, m_adapters[m_computeAdapterIndex].Get(),
            m_commandQueueExtensionEnabled, pOldCompute);
        delete pOldCompute;

        ShareHandles();

        m_commandQueueExtensionEnabled = m_pCompute->GetUsingIntelCommandQueueExtension();
    }

    // note: we can release() and create a new compute queue with/without extensions with no issues
    // render queue, we can't because of the tight relationship with the swap chain.
    if (changeQueueExtension)
    {
        m_pCompute->SetUseIntelCommandQueueExtension(m_commandQueueExtensionEnabled);
        m_commandQueueExtensionEnabled = m_pCompute->GetUsingIntelCommandQueueExtension() ||
            m_pRender->GetUsingIntelCommandQueueExtension();
    }

    // reset UI toggle history
    m_prevRenderAdapterIndex = m_renderAdapterIndex;
    m_prevComputeAdapterIndex = m_computeAdapterIndex;
    m_prevQueueExtension = m_commandQueueExtensionEnabled;
    m_prevFullScreen = m_fullScreen;
}
