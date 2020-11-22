// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/spin_lock.h"
#include "core/arm/cpu_interrupt_handler.h"
#include "core/core.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/scheduler.h"

namespace Kernel {

PhysicalCore::PhysicalCore(Core::System& system, std::size_t id, Kernel::Scheduler& scheduler,
                           Core::CPUInterruptHandler& interrupt_handler)
    : interrupt_handler{interrupt_handler},
      core_index{id}, scheduler{scheduler}, guard{std::make_unique<Common::SpinLock>()} {}

PhysicalCore::~PhysicalCore() = default;

void PhysicalCore::Idle() {
    interrupt_handler.AwaitInterrupt();
}

void PhysicalCore::Shutdown() {
    scheduler.Shutdown();
}

bool PhysicalCore::IsInterrupted() const {
    return interrupt_handler.IsInterrupted();
}

void PhysicalCore::Interrupt() {
    guard->lock();
    interrupt_handler.SetInterrupt(true);
    guard->unlock();
}

void PhysicalCore::ClearInterrupt() {
    guard->lock();
    interrupt_handler.SetInterrupt(false);
    guard->unlock();
}

} // namespace Kernel
