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
#include "defines.h"

#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl.h>
#include <sstream>

#include "D3D12GpuTimer.h"

using Microsoft::WRL::ComPtr;

class AdapterShared
{
public:
    AdapterShared();
    virtual ~AdapterShared();

    AdapterShared(const AdapterShared&) = delete;
    AdapterShared(AdapterShared&&) = delete;
    AdapterShared& operator=(const AdapterShared&) = delete;
    AdapterShared& operator=(AdapterShared&&) = delete;

    const auto& GetGpuTimes() const { return m_pTimer->GetTimes(); }

    // return if this adapter is using the intel command queue throttle extension
    bool GetUsingIntelCommandQueueExtension() const { return m_usingIntelCommandQueueExtension; }

    // stalls until adapter is idle
    virtual void WaitForGpu() = 0;

    // returns if this adapter uses unified memory (system memory is treated as local adapter memory)
    bool GetIsUMA() const { return m_isUMA; }

protected:
    // create a device with the highest feature support
    void CreateDevice(IDXGIAdapter1* in_pAdapter, ComPtr<ID3D12Device>& in_device);

    std::wstring GetAssetFullPath(const wchar_t* const in_filename);

    D3D12GpuTimer* m_pTimer;
    bool m_usingIntelCommandQueueExtension;

private:
    bool m_isUMA;
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline AdapterShared::AdapterShared()
    : m_pTimer(nullptr)
    , m_usingIntelCommandQueueExtension(false)
    , m_isUMA(false)
{
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline AdapterShared::~AdapterShared()
{
    delete m_pTimer;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void AdapterShared::CreateDevice(IDXGIAdapter1* in_pAdapter, ComPtr<ID3D12Device>& out_device)
{
    ThrowIfFailed(::D3D12CreateDevice(in_pAdapter, MINIMUM_D3D_FEATURE_LEVEL, IID_PPV_ARGS(&out_device)));

    // check for UMA support (uses system memory as local memory)
    D3D12_FEATURE_DATA_ARCHITECTURE featureData = {};
    const HRESULT hr = out_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &featureData, sizeof(featureData));
    m_isUMA = SUCCEEDED(hr) && featureData.UMA;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline std::wstring AdapterShared::GetAssetFullPath(const wchar_t* const in_filename)
{
    constexpr size_t PATHBUFFERSIZE = MAX_PATH * 4;
    TCHAR buffer[PATHBUFFERSIZE];
    ::GetCurrentDirectory(_countof(buffer), buffer);

    std::wostringstream assetFullPath;
    assetFullPath << buffer << L"\\\\" << in_filename;
    return assetFullPath.str();
}
