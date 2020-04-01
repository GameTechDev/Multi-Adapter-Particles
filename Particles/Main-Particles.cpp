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

#include "WindowProc.h"
#include "Particles.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib") // DXGI_DEBUG_ALL guuid
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPSTR /*lpCmdLine*/,
    _In_ int /*nCmdShow*/)
{
    WNDCLASSW wc = {};
    wc.lpszClassName = L"MultiGPU";
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WindowProc::WndProc;
    wc.cbWndExtra = 0;
    ::RegisterClassW(&wc);

    LONG windowDim = 1024;
    RECT windowRect = { 0, 0, windowDim, windowDim };
    ::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hWnd = ::CreateWindow(
        wc.lpszClassName, L"Particles",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        0, 0, hInstance, 0);
    if (!hWnd)
    {
        ::UnregisterClassW(wc.lpszClassName, hInstance);
        return -1;
    }

    ::ShowWindow(hWnd, SW_SHOWNORMAL);

    bool drawEnabled = true;

    ::SetWindowLongPtr(hWnd, GWLP_USERDATA, LONG_PTR(&drawEnabled));

    Particles particles(hWnd);

    MSG msg = { 0 };
    while (WM_QUIT != msg.message)
    {
        if (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
        else
        {
            if (drawEnabled)
            {
                particles.Draw();
            }
        }
    }

    particles.Shutdown();

    ::UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
