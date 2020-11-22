// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/microprofile.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/settings.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

/// Runs the GPU thread
static void RunThread(Core::System& system, VideoCore::RendererBase& renderer,
                      Core::Frontend::GraphicsContext& context, Tegra::DmaPusher& dma_pusher,
                      SynchState& state, Tegra::CDmaPusher& cdma_pusher) {
    std::string name = "yuzu:GPU";
    MicroProfileOnThreadCreate(name.c_str());
    Common::SetCurrentThreadName(name.c_str());
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    system.RegisterHostThread();

    // Wait for first GPU command before acquiring the window context
    while (state.queue.Empty())
        ;

    // If emulation was stopped during disk shader loading, abort before trying to acquire context
    if (!state.is_running) {
        return;
    }

    auto current_context = context.Acquire();

    CommandDataContainer next;
    while (state.is_running) {
        next = state.queue.PopWait();
        if (const auto submit_list = std::get_if<SubmitListCommand>(&next.data)) {
            dma_pusher.Push(std::move(submit_list->entries));
            dma_pusher.DispatchCalls();
        } else if (const auto command_list = std::get_if<SubmitChCommandEntries>(&next.data)) {
            // NVDEC
            cdma_pusher.Push(std::move(command_list->entries));
            cdma_pusher.DispatchCalls();
        } else if (const auto data = std::get_if<SwapBuffersCommand>(&next.data)) {
            renderer.SwapBuffers(data->framebuffer ? &*data->framebuffer : nullptr);
        } else if (std::holds_alternative<OnCommandListEndCommand>(next.data)) {
            renderer.Rasterizer().ReleaseFences();
        } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
            system.GPU().TickWork();
        } else if (const auto data = std::get_if<FlushRegionCommand>(&next.data)) {
            renderer.Rasterizer().FlushRegion(data->addr, data->size);
        } else if (const auto data = std::get_if<InvalidateRegionCommand>(&next.data)) {
            renderer.Rasterizer().OnCPUWrite(data->addr, data->size);
        } else if (std::holds_alternative<EndProcessingCommand>(next.data)) {
            return;
        } else {
            UNREACHABLE();
        }
        state.signaled_fence.store(next.fence);
    }
}

ThreadManager::ThreadManager(Core::System& system) : system{system} {}

ThreadManager::~ThreadManager() {
    if (!thread.joinable()) {
        return;
    }

    // Notify GPU thread that a shutdown is pending
    PushCommand(EndProcessingCommand());
    thread.join();
}

void ThreadManager::StartThread(VideoCore::RendererBase& renderer,
                                Core::Frontend::GraphicsContext& context,
                                Tegra::DmaPusher& dma_pusher, Tegra::CDmaPusher& cdma_pusher) {
    thread = std::thread(RunThread, std::ref(system), std::ref(renderer), std::ref(context),
                         std::ref(dma_pusher), std::ref(state), std::ref(cdma_pusher));
}

void ThreadManager::SubmitList(Tegra::CommandList&& entries) {
    PushCommand(SubmitListCommand(std::move(entries)));
}

void ThreadManager::SubmitCommandBuffer(Tegra::ChCommandHeaderList&& entries) {
    PushCommand(SubmitChCommandEntries(std::move(entries)));
}

void ThreadManager::SwapBuffers(const Tegra::FramebufferConfig* framebuffer) {
    PushCommand(SwapBuffersCommand(framebuffer ? std::make_optional(*framebuffer) : std::nullopt));
}

void ThreadManager::FlushRegion(VAddr addr, u64 size) {
    if (!Settings::IsGPULevelHigh()) {
        PushCommand(FlushRegionCommand(addr, size));
        return;
    }
    if (!Settings::IsGPULevelExtreme()) {
        return;
    }
    if (system.Renderer().Rasterizer().MustFlushRegion(addr, size)) {
        auto& gpu = system.GPU();
        u64 fence = gpu.RequestFlush(addr, size);
        PushCommand(GPUTickCommand());
        while (fence > gpu.CurrentFlushRequestFence()) {
        }
    }
}

void ThreadManager::InvalidateRegion(VAddr addr, u64 size) {
    system.Renderer().Rasterizer().OnCPUWrite(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(VAddr addr, u64 size) {
    // Skip flush on asynch mode, as FlushAndInvalidateRegion is not used for anything too important
    system.Renderer().Rasterizer().OnCPUWrite(addr, size);
}

void ThreadManager::WaitIdle() const {
    while (state.last_fence > state.signaled_fence.load(std::memory_order_relaxed)) {
    }
}

void ThreadManager::OnCommandListEnd() {
    PushCommand(OnCommandListEndCommand());
}

u64 ThreadManager::PushCommand(CommandData&& command_data) {
    const u64 fence{++state.last_fence};
    state.queue.Push(CommandDataContainer(std::move(command_data), fence));
    return fence;
}

} // namespace VideoCommon::GPUThread
