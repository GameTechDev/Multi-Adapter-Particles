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

#include <cassert>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "DXSampleHelper.h"
#include "igd12ext.h"

constexpr UINT INTEL_DEVICE_ID = 0x8086;

class ExtensionHelper
{
public:
    explicit ExtensionHelper(ID3D12Device* in_pDevice);
    ~ExtensionHelper();

    ExtensionHelper(const ExtensionHelper&) = delete;
    ExtensionHelper(ExtensionHelper&&) = delete;
    ExtensionHelper& operator=(const ExtensionHelper&) = delete;
    ExtensionHelper& operator=(ExtensionHelper&&) = delete;

    ID3D12CommandQueue* CreateCommandQueue(D3D12_COMMAND_QUEUE_DESC in_queueDesc);

    bool GetEnabled() const { return (nullptr != m_pExtensionContext); }

private:
    HMODULE m_extendionsHandle;
    INTC::ExtensionContext* m_pExtensionContext;
    INTC::PFNINTCDX12EXT_CREATECOMMANDQUEUE m_extCreateCommandQueue;

    void ReleaseExtensions();
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline ExtensionHelper::ExtensionHelper(ID3D12Device* in_pDevice)
    : m_extendionsHandle(nullptr)
    , m_pExtensionContext(nullptr)
    , m_extCreateCommandQueue(nullptr)
{
    // Check that this is a Intel device first.  Calling
    // CreateExtensionContext() on non-Intel devices can lead to issues

    bool intelDevice = false;

    {
        const LUID deviceLuid = in_pDevice->GetAdapterLuid();

        UINT flags = 0;
#ifdef _DEBUG
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

        IDXGIFactory2* factory2 = nullptr;
        ThrowIfFailed(::CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory2)));

        IDXGIAdapter* adapter = nullptr;
        for (UINT i = 0; factory2->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC desc;
            ThrowIfFailed(adapter->GetDesc(&desc));
            adapter->Release();

            if (desc.AdapterLuid.HighPart == deviceLuid.HighPart &&
                desc.AdapterLuid.LowPart == deviceLuid.LowPart)
            {
                intelDevice = (desc.VendorId == INTEL_DEVICE_ID);
                break;
            }
        }

        SAFE_RELEASE(factory2);
    }

    if (intelDevice)
    {
        m_extendionsHandle = INTC::D3D12LoadIntelExtensionsLibrary();
    
        auto CreateExtensionContext = (INTC::PFNINTCDX12EXT_D3D12CREATEDEVICEEXTENSIONCONTEXT) ::GetProcAddress(m_extendionsHandle, "D3D12CreateDeviceExtensionContext");
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

        //cleanup on fail
        ReleaseExtensions();
    }
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
    ID3D12CommandQueue* pCommandQueue = nullptr;

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
    if (m_extendionsHandle != nullptr)
    {
        if (m_pExtensionContext != nullptr)
        {
            auto DestroyExtensionContext = (INTC::PFNINTCDX12EXT_D3D12DESTROYDEVICEEXTENSIONCONTEXT) ::GetProcAddress(m_extendionsHandle, "D3D12DestroyDeviceExtensionContext");

            if (DestroyExtensionContext)
            {
                (*DestroyExtensionContext)(&m_pExtensionContext);
            }
        }

        const BOOL rv = ::FreeLibrary(m_extendionsHandle);
        assert(rv);
    }

    m_extendionsHandle = nullptr;
    m_pExtensionContext = nullptr;
    m_extCreateCommandQueue = nullptr;
}
