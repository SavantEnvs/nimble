// Shared setup for the Nimble fuzz harnesses.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "velox/common/memory/Memory.h"

namespace nimble_fuzz {

// Velox memory pools are the allocator behind every buffer Nimble decodes into. The root pool is
// capped so that a size field read from the input that asks for more than this is refused by the
// pool (a VeloxException, which the harnesses treat as "input rejected") instead of taking the
// whole process down. The cap is fixed, so the result does not depend on the host.
inline constexpr int64_t kPoolCapacityBytes = 1LL << 30;

// The process-wide root pool: created once, holds no per-input state.
inline facebook::velox::memory::MemoryPool& rootPool() {
  static std::shared_ptr<facebook::velox::memory::MemoryPool> root = [] {
    facebook::velox::memory::initializeMemoryManager(
        facebook::velox::memory::MemoryManager::Options{});
    return facebook::velox::memory::memoryManager()->addRootPool(
        "nimble_fuzz_root", kPoolCapacityBytes);
  }();
  return *root;
}

// A fresh leaf pool for one input; everything decoded from the input is freed with it.
inline std::shared_ptr<facebook::velox::memory::MemoryPool> newLeafPool() {
  return rootPool().addLeafChild("input");
}

} // namespace nimble_fuzz
