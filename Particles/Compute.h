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

#include "AdapterShared.h"
#include <DirectXMath.h>

class ExtensionHelper;

class Compute : public AdapterShared
{
public:
    Compute(UINT in_numParticles,
        IDXGIAdapter1* in_pAdapter,
        bool in_useIntelCommandQueueExtension,
        Compute* in_pCompute = 0);
    virtual ~Compute();

    Compute(const Compute&) = delete;
    Compute(Compute&&) = delete;
    Compute& operator=(const Compute&) = delete;
    Compute& operator=(Compute&&) = delete;

    // input is fence value of other adapter. waits to overwrite shared buffer.
    void Simulate(int in_numActiveParticles, UINT64 in_sharedFenceValue);

    // changes extension setting only if different from current setting
    void SetUseIntelCommandQueueExtension(bool in_desiredSetting);

    // provide cross-adapter shared handles to copy particle buffers to
    struct SharedHandles
    {
        HANDLE m_heap;
        HANDLE m_fence;

        UINT64 m_alignedDataSize;
        UINT m_bufferIndex;
    };
    const SharedHandles& GetSharedHandles(HANDLE in_fenceHandle);

    UINT64 GetFenceValue() const { return m_fenceValue; }

    struct ParticleVelocity
    {
        DirectX::XMFLOAT3 velocity;
    };

    // stalls until adapter is idle
    virtual void WaitForGpu() override;

    void SetAsync(
        ComPtr<ID3D12Fence> in_fence,
        ComPtr<ID3D12Resource>* in_buffers,
        UINT in_bufferIndex);
    void ResetFromAsyncHelper();
private:
    static constexpr UINT m_NUM_BUFFERS = 2;

    static constexpr float ParticleSpread = PARTICLE_SPREAD;
    const UINT m_numParticles;

    ExtensionHelper* m_pExtensionHelper;

    ComPtr<ID3D12Device> m_device;

    // compute command queue
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12CommandAllocator> m_commandAllocators[m_NUM_BUFFERS];

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_computeState;
    ComPtr<ID3D12Resource> m_constantBufferCS;

    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    UINT m_srvUavDescriptorSize;

    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent;

    UINT m_bufferIndex;

    ComPtr<ID3D12Heap> m_sharedHeap;
    ComPtr<ID3D12Resource> m_velocityBuffers[m_NUM_BUFFERS];
    ComPtr<ID3D12Resource> m_positionBuffers[m_NUM_BUFFERS];
    SharedHandles m_sharedHandles;

    void Initialize(IDXGIAdapter1* in_pAdapter);
    void CreateCommandQueue();
    void CreateSharedBuffers();

    // initialize particle positions. Only need to do this once.
    // on subsequent compute destroy/create, can copy old state from render object
    void InitializeParticles();

    UINT64 m_frameFenceValues[m_NUM_BUFFERS];
    UINT64 m_fenceValue;

    // sample code waited in this method
    // this version returns a handle, so the calling function can WaitOn/Multiple/
    void MoveToNextFrame();
    ComPtr<ID3D12Fence> m_sharedRenderFence;

    void CopyState(Compute* in_pCompute);

    // shenanigans to simplify transitioning /out/ of async compute mode
    ComPtr<ID3D12Resource> m_sharedComputeBuffersReference[m_NUM_BUFFERS];
};
