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
#include <d3d12.h>
#include <dxgi1_6.h>

#include "DXSampleHelper.h"
#include "igd12ext.h"

class ExtensionHelper
{
public:
    ExtensionHelper(ID3D12Device* in_pDevice);
    ~ExtensionHelper();

    ID3D12CommandQueue* CreateCommandQueue(D3D12_COMMAND_QUEUE_DESC in_queueDesc);

    bool GetEnabled() const { return (nullptr != m_pExtensionContext); }
private:
    ID3D12Device* m_pDevice;

    INTC::PFNINTCDX12EXT_CREATECOMMANDQUEUE m_extCreateCommandQueue;
    HMODULE m_extendionsHandle;
    INTC::ExtensionContext* m_pExtensionContext;

    void ReleaseExtensions();
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline ExtensionHelper::ExtensionHelper(ID3D12Device* in_pDevice)
{
    m_pDevice = in_pDevice;

    m_extendionsHandle = INTC::D3D12LoadIntelExtensionsLibrary();
    m_pExtensionContext = nullptr;
    m_extCreateCommandQueue = nullptr;

    // Check that this is a Intel device first.  Calling
    // CreateExtensionContext() on non-Intel devices can lead to issues

    bool intelDevice = false;
    {
        LUID deviceLuid = in_pDevice->GetAdapterLuid();

        IDXGIFactory* factory = nullptr;
        ThrowIfFailed(CreateDXGIFactory(IID_PPV_ARGS(&factory)));
        IDXGIAdapter* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC desc;
            adapter->GetDesc(&desc);
            adapter->Release();

            if (desc.AdapterLuid.HighPart == deviceLuid.HighPart &&
                desc.AdapterLuid.LowPart == deviceLuid.LowPart)
            {
                intelDevice = (desc.VendorId == 0x8086);
                break;
            }
        }
    }

    if (intelDevice)
    {
        auto CreateExtensionContext = (INTC::PFNINTCDX12EXT_D3D12CREATEDEVICEEXTENSIONCONTEXT) GetProcAddress(m_extendionsHandle, "D3D12CreateDeviceExtensionContext");
        if (CreateExtensionContext != nullptr)
        {
            INTC::ExtensionInfo info = {};
            info.requestedExtensionVersion.Version.Major = 1;
            info.requestedExtensionVersion.Version.Minor = 0;
            info.requestedExtensionVersion.Version.Revision = 1;

            INTC::D3D12_EXTENSION_FUNCS_01000001 funcs = {};
            auto pFuncs = &funcs;
            auto hr = (*CreateExtensionContext)(in_pDevice, &m_pExtensionContext, (void**)&pFuncs, sizeof(funcs), &info, nullptr);
            if (SUCCEEDED(hr) &&
                info.returnedExtensionVersion.Version.Revision >= 1)
            {
                m_extCreateCommandQueue = funcs.CreateCommandQueue;
                return;
            }
        }
    }

    ReleaseExtensions();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline ExtensionHelper::~ExtensionHelper()
{
    ReleaseExtensions();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline ID3D12CommandQueue* ExtensionHelper::CreateCommandQueue(D3D12_COMMAND_QUEUE_DESC in_queueDesc)
{
    ID3D12CommandQueue* pCommandQueue = 0;

    if (nullptr != m_pExtensionContext)
    {
        // This version of the command throttle extension works at create time
        INTC::D3D12_COMMAND_QUEUE_DESC extDesc = {};
        extDesc.pD3D12Desc = &in_queueDesc;
        extDesc.CommandThrottlePolicy = INTC::D3D12_COMMAND_QUEUE_THROTTLE_MAX_PERFORMANCE;

        ThrowIfFailed((*m_extCreateCommandQueue)(m_pExtensionContext, &extDesc,
            IID_PPV_ARGS(&pCommandQueue)));
    }
    return pCommandQueue;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void ExtensionHelper::ReleaseExtensions()
{
    if (m_extendionsHandle != NULL)
    {
        if (m_pExtensionContext != nullptr)
        {
            auto DestroyExtensionContext = (INTC::PFNINTCDX12EXT_D3D12DESTROYDEVICEEXTENSIONCONTEXT) GetProcAddress(m_extendionsHandle, "D3D12DestroyDeviceExtensionContext");

            if (DestroyExtensionContext)
            {
                (*DestroyExtensionContext)(&m_pExtensionContext);
            }
        }

        FreeLibrary(m_extendionsHandle);
    }

    m_extendionsHandle = nullptr;
    m_pExtensionContext = nullptr;
    m_extCreateCommandQueue = nullptr;
}
