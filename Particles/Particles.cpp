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

#include <d3d12.h>
#include <Windows.h>
#include <algorithm> // for std::min()

#include "Particles.h"
#include "d3dx12.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

using Microsoft::WRL::ComPtr;

const INT ParticleCount = 4 * 1024 * 1024;

//-----------------------------------------------------------------------------
// Enable the D3D12 debug layer.
//-----------------------------------------------------------------------------
void InitDebugLayer()
{
    {
        ID3D12Debug1* pDebugController = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
        {
            //pDebugController->SetEnableGPUBasedValidation(TRUE);
            pDebugController->EnableDebugLayer();
            pDebugController->Release();
        }
    }
}

//-----------------------------------------------------------------------------
// share handles between render and compute
// optionally, copy particle state from render to compute (usually compute creates particle state)
//-----------------------------------------------------------------------------
void Particles::ShareHandles()
{
    HANDLE renderFenceHandle = m_pRender->GetSharedFenceHandle();
    auto sharedHandles = m_pCompute->GetSharedHandles(renderFenceHandle);
    m_pRender->SetShared(sharedHandles);
}

//-----------------------------------------------------------------------------
// discover adapters
// save info so roles can be dynamically changed
//-----------------------------------------------------------------------------
Particles::Particles(HWND in_hwnd)
{
    m_hwnd = in_hwnd;
    m_vsyncEnabled = true;
    m_fullScreen = false;
    m_frameTime = 0;
    m_particleSize = INITIAL_PARTICLE_SIZE;
    m_particleIntensity = INITIAL_PARTICLE_INTENSITY;

    m_numParticlesRendered = ParticleCount;
    m_numParticlesCopied = ParticleCount;
    m_numParticlesSimulated = ParticleCount;
    m_numParticlesLinked = true;

    m_windowInfo.cbSize = sizeof(WINDOWINFO);
    GetWindowInfo(m_hwnd, &m_windowInfo);

    InitDebugLayer();

    ComPtr<IDXGIFactory2> factory = nullptr;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory))))
    {
        flags &= ~DXGI_CREATE_FACTORY_DEBUG;
        ThrowIfFailed(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)));
    }
    ThrowIfFailed(factory.As<IDXGIFactory4>(&m_dxgiFactory));

    // find adapters
    ComPtr<IDXGIAdapter1> adapter = nullptr;
    for (UINT i = 0; m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) && (desc.VendorId != 5140))
        {
            m_adapters.push_back(adapter);

            std::string narrowString;
            int numChars = WideCharToMultiByte(CP_UTF8, 0, desc.Description, _countof(desc.Description), NULL, 0, NULL, NULL);
            narrowString.resize(numChars);
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, (int)narrowString.size(), &narrowString[0], numChars, NULL, NULL);

            // m_adapterDescriptions holds the strings
            m_adapterDescriptions.push_back(narrowString);
            // m_adapterDescriptionPtrs is an array of pointers into m_adapterDescriptions, and is used by imgui
            m_adapterDescriptionPtrs.push_back(m_adapterDescriptions.back().c_str());
        }
        adapter = nullptr;
    }

    // initial state
    size_t numAdapters = m_adapters.size();
    if (m_adapters.size())
    {
        m_renderAdapterIndex = 0;
        // FIXME
        m_computeAdapterIndex = int(numAdapters - 1);

        m_pRender = new Render(m_hwnd, ParticleCount, m_adapters[m_renderAdapterIndex], m_commandQueueExtensionEnabled, m_fullScreen, m_windowInfo.rcClient);
        m_pCompute = new Compute(ParticleCount, m_adapters[m_computeAdapterIndex], m_commandQueueExtensionEnabled);

        ShareHandles();

        m_commandQueueExtensionEnabled = m_pCompute->GetUsingIntelCommandQueueExtension() ||
            m_pRender->GetUsingIntelCommandQueueExtension();
    }
    else
    {
        throw;
    }

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

    // start frame duration timer
    m_frameTimer.Start();
    m_previousFrameTime = (float)m_frameTimer.GetTime();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
Particles::~Particles()
{
    delete m_pCompute;
    delete m_pRender;
    ImGui::DestroyContext(nullptr);
}


//-----------------------------------------------------------------------------
// Initialize UI resources
// create SRV heap and root signature
// device comes from the Render object
//-----------------------------------------------------------------------------
void Particles::InitGui()
{
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

    ImGui::SliderInt("Rendered", numParticlesRendered, std::min<int>(MIN_NUM_PARTICLES, ParticleCount), ParticleCount);
    ImGui::SliderInt("Copied", numParticlesCopied, std::min<int>(MIN_NUM_PARTICLES, ParticleCount), ParticleCount);
    ImGui::SliderInt("Simulated", numParticlesSimulated, std::min<int>(MIN_NUM_PARTICLES, ParticleCount), ParticleCount);
    ImGui::Checkbox("Link Sliders", &m_numParticlesLinked);

    //-----------------------------------------------------
    // timers
    //-----------------------------------------------------
    ImGui::Separator();

    for (const auto& t : m_pRender->GetGpuTimes())
    {
        ImGui::Text("%s: %f", t.second.c_str(), t.first * 1000.0f);
    }
    for (const auto& t : m_pCompute->GetGpuTimes())
    {
        ImGui::Text("%s: %f", t.second.c_str(), t.first * 1000.0f);
    }
    ImGui::Text("frameTime: %f", m_frameTime);
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
    m_pRender->SetParticleSize(m_particleSize);
    m_pRender->SetParticleIntensity(m_particleIntensity);

    const int prevRenderAdapterIndex = m_renderAdapterIndex;
    const int prevComputeAdapterIndex = m_computeAdapterIndex;
    const bool prevQueueExtension = m_commandQueueExtensionEnabled;
    const bool prevFullScreen = m_fullScreen;

    if (m_numParticlesLinked)
    {
        m_numParticlesCopied = m_numParticlesRendered;
        m_numParticlesSimulated = m_numParticlesRendered;
    }

    // start simulation. This also starts copy of results for next frame
    UINT64 renderSharedFenceValue = m_pCompute->GetFenceValue();
    HANDLE drawHandle = m_pRender->Draw(m_numParticlesRendered, this, renderSharedFenceValue, m_numParticlesCopied);
    m_pCompute->Simulate(m_numParticlesSimulated, renderSharedFenceValue);

    // because the command lists of each adapter wait() on each other,
    // only need to host-wait() around the Present() on the render adapter
    if (drawHandle)
    {
        WaitForSingleObjectEx(drawHandle, INFINITE, FALSE);
    }

    // if anything changed that might result in an adapter being removed,
    // drain all the pipelines
    if (
        (prevRenderAdapterIndex != m_renderAdapterIndex)
        || (prevComputeAdapterIndex != m_computeAdapterIndex)
        || (prevQueueExtension != m_commandQueueExtensionEnabled)
        || (prevFullScreen != m_fullScreen)
        )
    {
        m_pRender->WaitForGpu();
        m_pCompute->WaitForGpu();
    }

    // host-side frame time
    float currentFrameTime = (float)m_frameTimer.GetTime();
    float frameTime = 1000.0f * (currentFrameTime - m_previousFrameTime);
    m_previousFrameTime = currentFrameTime;
    const std::uint32_t AVERAGE_OVER = 20;
    m_frameTime *= (AVERAGE_OVER - 1);
    m_frameTime += frameTime;
    m_frameTime /= AVERAGE_OVER;

    //-----------------------------------------------------
    // Handle GUI changes
    //-----------------------------------------------------

    // switching from windowed to full screen? remember window state
    if (m_fullScreen && !prevFullScreen)
    {
        GetWindowInfo(m_hwnd, &m_windowInfo);
    }

    // new render device?
    // this became more complicated because changing the render queue (by enabling extension) requires reset of swapchain
    // added some extra logic to check if the render doesn't support the extension, because reset of full-screen is annoying
    if (
        // new adapter? need to create new Render
        (prevRenderAdapterIndex != m_renderAdapterIndex) ||
        // change to/from full screen? need to create new Render
        (prevFullScreen != m_fullScreen) ||
        // change of queue extension state on renderer that supports it? need to create new Render
        ((prevQueueExtension != m_commandQueueExtensionEnabled) && (m_pRender->GetSupportsIntelCommandQueueExtension()))
        )
    {
        delete m_pRender;

        // for windowed mode, reset the window style and position before creating new Render
        if (prevFullScreen && !m_fullScreen)
        {
            UINT width = m_windowInfo.rcWindow.right - m_windowInfo.rcWindow.left;
            UINT height = m_windowInfo.rcWindow.bottom - m_windowInfo.rcWindow.top;
            UINT left = m_windowInfo.rcWindow.left;
            UINT top = m_windowInfo.rcWindow.top;
            SetWindowLongPtr(m_hwnd, GWL_STYLE, m_windowInfo.dwStyle);
            SetWindowPos(m_hwnd, HWND_NOTOPMOST, left, top, width, height, SWP_FRAMECHANGED);
        }

        m_pRender = new Render(m_hwnd, ParticleCount, m_adapters[m_renderAdapterIndex], m_commandQueueExtensionEnabled, m_fullScreen, m_windowInfo.rcClient);
#if IMGUI_ENABLED
        InitGui();
#endif

        ShareHandles();
    }

    if (prevComputeAdapterIndex != m_computeAdapterIndex)
    {
        Compute* pOldCompute = m_pCompute;
        m_pCompute = new Compute(ParticleCount, m_adapters[m_computeAdapterIndex],
            m_commandQueueExtensionEnabled, pOldCompute);
        delete pOldCompute;

        ShareHandles();

        m_commandQueueExtensionEnabled = m_pCompute->GetUsingIntelCommandQueueExtension();
    }

    if (prevQueueExtension != m_commandQueueExtensionEnabled)
    {
        m_pCompute->SetUseIntelCommandQueueExtension(m_commandQueueExtensionEnabled);
        m_commandQueueExtensionEnabled = m_pCompute->GetUsingIntelCommandQueueExtension() ||
            m_pRender->GetUsingIntelCommandQueueExtension();
    }
}
