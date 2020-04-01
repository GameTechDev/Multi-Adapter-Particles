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
#include <wrl.h>

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

#include "d3dx12.h"
#include "DXSampleHelper.h"

class D3D12GpuTimer
{
public:
    D3D12GpuTimer(
        ID3D12Device* in_pDevice, // required to create internal resources
        ID3D12CommandQueue* in_pCommandQueue, // required for frequency query
        std::uint32_t in_numTimers, std::uint32_t in_averageOver = 20);

    void SetTimerName(std::uint32_t in_index, const std::string& in_name);

    void BeginTimer(ID3D12GraphicsCommandList* in_pCommandList, std::uint32_t in_index);
    void EndTimer(ID3D12GraphicsCommandList* in_pCommandList, std::uint32_t in_index);

    void ResolveAllTimers(ID3D12GraphicsCommandList* in_pCommandList);

    typedef std::vector<std::pair<float, std::string>> TimeArray;
    const TimeArray& GetTimes() const { return m_times; }

private:
    std::uint32_t m_numTimers;   // how many we expose. we need double to record begin + end
    std::uint32_t m_totalTimers;
    TimeArray m_times;
    std::uint64_t m_gpuFrequency;
    const std::uint32_t m_averageOver;

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline D3D12GpuTimer::D3D12GpuTimer(
    ID3D12Device* in_pDevice,
    ID3D12CommandQueue* in_pCommandQueue,
    std::uint32_t in_numTimers,
    std::uint32_t in_averageOver)
    : m_numTimers(in_numTimers)
    , m_totalTimers(in_numTimers * 2) // begin + end, so we can take a difference
    , m_gpuFrequency(0)
    , m_averageOver(in_averageOver)
    , m_commandQueue(in_pCommandQueue)
{
    m_times.resize(m_numTimers);

    const UINT64 bufferSize = m_totalTimers * sizeof(UINT64);

    ThrowIfFailed(in_pDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_buffer)));
    m_buffer->SetName(L"GPUTimeStamp Buffer");

    D3D12_QUERY_HEAP_DESC QueryHeapDesc = {};
    QueryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    QueryHeapDesc.Count = m_totalTimers;

    ThrowIfFailed(in_pDevice->CreateQueryHeap(&QueryHeapDesc, IID_PPV_ARGS(&m_heap)));
    m_heap->SetName(L"GpuTimeStamp QueryHeap");

    ThrowIfFailed(in_pCommandQueue->GetTimestampFrequency(&m_gpuFrequency));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void D3D12GpuTimer::SetTimerName(std::uint32_t in_index, const std::string& in_name)
{
    if (in_index < m_times.size())
    {
        m_times[in_index].second = in_name;
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void D3D12GpuTimer::BeginTimer(ID3D12GraphicsCommandList* in_pCommandList, std::uint32_t in_index)
{
    const UINT index = in_index * 2;
    in_pCommandList->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void D3D12GpuTimer::EndTimer(ID3D12GraphicsCommandList* in_pCommandList, std::uint32_t in_index)
{
    const UINT index = (in_index * 2) + 1;
    in_pCommandList->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void D3D12GpuTimer::ResolveAllTimers(ID3D12GraphicsCommandList* in_pCommandList)
{
    in_pCommandList->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_totalTimers, m_buffer.Get(), 0);
    // FIXME? gpu frequency can fluctuate over time. Does this query reflect current clock rate?
    ThrowIfFailed(m_commandQueue->GetTimestampFrequency(&m_gpuFrequency));

    void* pData = nullptr;
    ThrowIfFailed(m_buffer->Map(0, &CD3DX12_RANGE(0, m_totalTimers), &pData));

    const UINT64* pTimestamps = reinterpret_cast<UINT64*>(pData);
    for (std::uint32_t i = 0; i < m_numTimers; i++)
    {
        UINT64 deltaTime = pTimestamps[1] - pTimestamps[0];
        if (pTimestamps[1] < pTimestamps[0])
        {
            deltaTime = pTimestamps[0] - pTimestamps[1];
        }

        const float delta = float(deltaTime) / float(m_gpuFrequency);
        const float t = m_times[i].first * (m_averageOver - 1);
        m_times[i].first = (t + delta) / m_averageOver;

        pTimestamps += 2;
    }

    // Unmap with an empty range (written range).
    m_buffer->Unmap(0, &CD3DX12_RANGE());
}
