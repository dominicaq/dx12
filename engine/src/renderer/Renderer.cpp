#include "Renderer.h"

Renderer::Renderer(HWND hwnd, const EngineConfig& config) {
    m_device = std::make_unique<DX12Device>();
    if (!m_device->Initialize(config.enableDebugLayer)) {
        throw std::runtime_error("Failed to initialize device");
    }

    // Create command queue manager
    m_commandManager = std::make_unique<CommandQueueManager>();
    if (!m_commandManager->Initialize(m_device->GetDevice())) {
        throw std::runtime_error("Failed to create command queues");
    }

    // Create resource manager
    m_resourceManager = std::make_unique<ResourceManager>();
    if (!m_resourceManager->Initialize(m_device.get(), m_commandManager->GetGraphicsQueue()->GetCommandQueue())) {
        throw std::runtime_error("Failed to initialize resource manager");
    }

    // Create SwapChain - use graphics queue
    m_swapChain = std::make_unique<SwapChain>(
        m_device.get(),
        m_commandManager->GetGraphicsQueue()->GetCommandQueue(),
        hwnd,
        config.windowWidth,
        config.windowHeight,
        config.backBufferCount
    );
    if (!m_swapChain->IsInitialized()) {
        throw std::runtime_error("Failed to create swap chain");
    }

    // Initialize per-frame resources
    if (!InitializeFrameResources()) {
        throw std::runtime_error("Failed to initialize frame resources");
    }

    // Create shared command list
    m_commandList = std::make_unique<CommandList>();
    if (!m_commandList->Initialize(m_device->GetDevice(),
                                   m_frameResources[0].commandAllocator.get(),
                                   D3D12_COMMAND_LIST_TYPE_DIRECT)) {
        throw std::runtime_error("Failed to create command list");
    }
}

Renderer::~Renderer() {
    if (m_device && m_commandManager) {
        WaitForAllFrames();
    }
    ReleaseFrameResources();
}

bool Renderer::InitializeFrameResources() {
    UINT bufferCount = m_swapChain->GetBufferCount();
    m_frameResources.resize(bufferCount);

    for (UINT i = 0; i < bufferCount; i++) {
        FrameResources& frame = m_frameResources[i];

        // Create command allocator for this frame
        frame.commandAllocator = std::make_unique<CommandAllocator>();
        if (!frame.commandAllocator->Initialize(m_device->GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT)) {
            printf("Failed to create command allocator for frame %u\n", i);
            return false;
        }

        // Create fence for this frame
        HRESULT hr = m_device->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                        IID_PPV_ARGS(&frame.frameFence));
        if (FAILED(hr)) {
            printf("Failed to create fence for frame %u: 0x%08X\n", i, hr);
            return false;
        }

        // Create fence event
        frame.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!frame.fenceEvent) {
            printf("Failed to create fence event for frame %u\n", i);
            return false;
        }

#ifdef _DEBUG
        std::wstring fenceName = L"Frame " + std::to_wstring(i) + L" Fence";
        frame.frameFence->SetName(fenceName.c_str());
#endif
    }

    return true;
}

void Renderer::ReleaseFrameResources() {
    m_frameResources.clear();
}

CommandList* Renderer::BeginFrame() {
    m_resourceManager->BeginFrame();

    // Get current frame index from swap chain
    m_currentFrameIndex = m_swapChain->GetCurrentBackBufferIndex();
    FrameResources& currentFrame = m_frameResources[m_currentFrameIndex];

    // Wait for this frame to be available (if we're cycling through faster than GPU can keep up)
    WaitForFrame(m_currentFrameIndex);

    // Reset command allocator for current frame
    if (!currentFrame.commandAllocator->Reset()) {
        printf("Failed to reset command allocator for frame %u\n", m_currentFrameIndex);
        return nullptr;
    }

    // Reset command list with current frame's allocator
    if (!m_commandList->Reset(currentFrame.commandAllocator.get())) {
        printf("Failed to reset command list for frame %u\n", m_currentFrameIndex);
        return nullptr;
    }

    // Transition back buffer to render target
    ID3D12Resource* backBuffer = m_swapChain->GetCurrentBackBuffer();
    m_commandList->TransitionBarrier(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    return m_commandList.get();
}

void Renderer::EndFrame(const EngineConfig& config) {
    FrameResources& currentFrame = m_frameResources[m_currentFrameIndex];

    // Transition back buffer to present
    ID3D12Resource* backBuffer = m_swapChain->GetCurrentBackBuffer();
    m_commandList->TransitionBarrier(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    if (!m_commandList->Close()) {
        printf("Failed to close command list for frame %u\n", m_currentFrameIndex);
        return;
    }

    // Execute commands
    ID3D12CommandList* commandLists[] = { m_commandList->GetCommandList() };
    m_commandManager->GetGraphicsQueue()->ExecuteCommandLists(1, commandLists);

    // Signal fence for current frame with unique value
    const UINT64 currentFenceValue = m_nextFenceValue++;
    currentFrame.fenceValue = currentFenceValue;

    HRESULT hr = m_commandManager->GetGraphicsQueue()->GetCommandQueue()->Signal(
        currentFrame.frameFence.Get(), currentFenceValue);
    if (FAILED(hr)) {
        printf("Failed to signal fence for frame %u: 0x%08X\n", m_currentFrameIndex, hr);
    }

    m_resourceManager->EndFrame();

    // Present the frame
    m_swapChain->Present(config.vsync);
}

void Renderer::WaitForFrame(UINT frameIndex) {
    if (frameIndex >= m_frameResources.size()) {
        return;
    }

    FrameResources& frame = m_frameResources[frameIndex];

    // If no fence value set, frame hasn't been used yet
    if (frame.fenceValue == 0) {
        return;
    }

    // Check if frame is already complete
    if (frame.frameFence->GetCompletedValue() >= frame.fenceValue) {
        return;
    }

    // Wait for frame completion
    HRESULT hr = frame.frameFence->SetEventOnCompletion(frame.fenceValue, frame.fenceEvent);
    if (FAILED(hr)) {
        printf("SetEventOnCompletion failed for frame %u: 0x%08X\n", frameIndex, hr);
        return;
    }

    WaitForSingleObject(frame.fenceEvent, INFINITE);
}

void Renderer::WaitForAllFrames() {
    for (UINT i = 0; i < m_frameResources.size(); i++) {
        WaitForFrame(i);
    }
}

bool Renderer::IsFrameComplete(UINT frameIndex) const {
    if (frameIndex >= m_frameResources.size()) {
        return true;
    }

    const FrameResources& frame = m_frameResources[frameIndex];

    // If no fence value set, frame hasn't been used yet
    if (frame.fenceValue == 0) {
        return true;
    }

    return frame.frameFence->GetCompletedValue() >= frame.fenceValue;
}

void Renderer::ClearBackBuffer(CommandList* cmdList, float clearColor[4]) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain->GetCurrentBackBufferRTV();
    cmdList->ClearRenderTarget(rtv, clearColor);
}

void Renderer::OnReconfigure(UINT width, UINT height, UINT bufferCount) {
    if (!m_swapChain) {
        return;
    }

    // Wait for all frames before reconfiguring
    WaitForAllFrames();

    // If buffer count is changing, we need to recreate frame resources
    UINT oldBufferCount = m_swapChain->GetBufferCount();
    UINT newBufferCount = (bufferCount == 0) ? oldBufferCount : bufferCount;

    if (!m_swapChain->Reconfigure(width, height, bufferCount)) {
        printf("Failed to reconfigure swap chain\n");
        return;
    }

    // Recreate frame resources if buffer count changed
    if (newBufferCount != oldBufferCount) {
        ReleaseFrameResources();
        if (!InitializeFrameResources()) {
            printf("Failed to reinitialize frame resources after reconfigure\n");
        }
    }
}
