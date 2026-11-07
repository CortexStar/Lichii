// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace VideoCommon {

inline std::atomic<unsigned> shader_cache_critical_sections{};
inline std::atomic<std::int64_t> shader_cache_last_progress_ms{};

inline std::int64_t ShaderCacheClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void ShaderCacheWriteProgress() {
    shader_cache_last_progress_ms.store(ShaderCacheClockMs(), std::memory_order_relaxed);
}

constexpr bool ShaderCacheWriteNeedsGrace(unsigned active, std::int64_t elapsed_ms) {
    return active != 0 && elapsed_ms < 120'000;
}

inline bool WaitForShaderCacheWrite() {
    const auto active = shader_cache_critical_sections.load(std::memory_order_acquire);
    const auto elapsed = ShaderCacheClockMs() - shader_cache_last_progress_ms.load(std::memory_order_relaxed);
    return ShaderCacheWriteNeedsGrace(active, elapsed);
}

class ShaderCacheCriticalScope {
public:
    ShaderCacheCriticalScope() {
        ShaderCacheWriteProgress();
        shader_cache_critical_sections.fetch_add(1, std::memory_order_release);
    }
    ~ShaderCacheCriticalScope() {
        shader_cache_critical_sections.fetch_sub(1, std::memory_order_release);
    }
    ShaderCacheCriticalScope(const ShaderCacheCriticalScope&) = delete;
    ShaderCacheCriticalScope& operator=(const ShaderCacheCriticalScope&) = delete;
};

} // namespace VideoCommon
