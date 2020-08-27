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
#pragma once

#include "WindowProc.h"

#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include <cstdint>
#include <vector>
#include <string>

#include "Timer.h"

class Render;
class Compute;

using Microsoft::WRL::ComPtr;

class Particles
{
public:
    explicit Particles(HWND in_hwnd);
    ~Particles();

    void Draw();
    void DrawGUI(ID3D12GraphicsCommandList* in_pCommandList);
    bool GetVsyncEnabled() const { return m_vsyncEnabled; }

    void Shutdown();

private:
    HWND m_hwnd;

    Render* m_pRender;
    Compute* m_pCompute;

    int m_renderAdapterIndex;
    int m_computeAdapterIndex;
    bool m_commandQueueExtensionEnabled;
    bool m_vsyncEnabled;
    bool m_fullScreen;

    // used to create device & resize swap chain
    ComPtr<IDXGIFactory2> m_dxgiFactory;
    std::vector< ComPtr<IDXGIAdapter1> > m_adapters;
    std::vector<std::string> m_adapterDescriptions;
    std::vector<const char*> m_adapterDescriptionPtrs;

    std::uint32_t m_height;
    TimerAverageOver m_frameTimer;
 
    enum RootParameters : UINT32
    {
        //RootCBV = 0,
        RootSRVTable,
        RootParametersCount
    };
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    void InitGui();

    void ShareHandles();

    WINDOWINFO m_windowInfo;
    float m_particleSize;
    float m_particleIntensity;

    int m_numParticlesRendered;
    int m_numParticlesCopied;
    int m_numParticlesSimulated;
    bool m_numParticlesLinked;

    // try to pick an initial state with compute->integrated and render->discrete
    void AssignAdapters();

    int m_maxNumParticles;
    bool m_enableUI;
    bool m_enableExtensions;
    void ParseCommandLine();

    // UI toggle history
    int m_prevRenderAdapterIndex;
    int m_prevComputeAdapterIndex;
    bool m_prevQueueExtension;
    bool m_prevFullScreen;
};
