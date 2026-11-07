// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>

#include "video_core/renderer_vulkan/static_compute_match.h"
#include "video_core/renderer_vulkan/pipeline_cache_policy.h"
#include "video_core/shader_cache_io.h"

int main() try {
    static_assert(!Vulkan::NeedsNupBlendDepthVariant(true, true));
    static_assert(Vulkan::NeedsNupBlendDepthVariant(false, true));
    static_assert(Vulkan::NeedsNupBlendDepthVariant(true, false));
    static_assert(Vulkan::NeedsNupBlendDepthVariant(false, false));
    static_assert(VideoCommon::ShaderCacheWriteNeedsGrace(1, 119999));
    static_assert(!VideoCommon::ShaderCacheWriteNeedsGrace(1, 120000));
    static_assert(!VideoCommon::ShaderCacheWriteNeedsGrace(0, 0));
    {
        VideoCommon::ShaderCacheCriticalScope outer;
        {
            VideoCommon::ShaderCacheCriticalScope inner;
            if (VideoCommon::shader_cache_critical_sections.load() != 2 ||
                !VideoCommon::WaitForShaderCacheWrite()) {
                throw std::runtime_error("nested cache writes lost shutdown protection");
            }
        }
        if (VideoCommon::shader_cache_critical_sections.load() != 1) {
            throw std::runtime_error("inner cache write released outer protection");
        }
    }
    if (VideoCommon::WaitForShaderCacheWrite()) {
        throw std::runtime_error("finished cache write retained shutdown protection");
    }

    static_assert(Vulkan::UseApplicationPipelineCache(true, true, true));
    static_assert(!Vulkan::UseApplicationPipelineCache(false, true, true));
    static_assert(!Vulkan::UseApplicationPipelineCache(true, false, true));
    static_assert(Vulkan::UseApplicationPipelineCache(true, false, false));
    static_assert(!Vulkan::FlushPipelineCacheDuringGameplay(true, true));
    static_assert(Vulkan::FlushPipelineCacheDuringGameplay(false, true));
    using ReadResult = Vulkan::CacheReadResult;
    const auto query_failure = Vulkan::ReadPipelineCacheSnapshot([](size_t*, void*) {
        return ReadResult::Failed;
    });
    if (query_failure) throw std::runtime_error("failed cache query was accepted");
    const auto data_failure = Vulkan::ReadPipelineCacheSnapshot([](size_t* size, void* data) {
        *size = 4;
        return data ? ReadResult::Failed : ReadResult::Complete;
    });
    if (data_failure) throw std::runtime_error("failed cache read was accepted");
    unsigned reads = 0;
    const auto retried = Vulkan::ReadPipelineCacheSnapshot([&](size_t* size, void* data) {
        *size = 4;
        if (!data) return ReadResult::Complete;
        if (++reads == 1) return ReadResult::Retry;
        std::memcpy(data, "warm", 4);
        return ReadResult::Complete;
    });
    if (!retried || std::string(retried->begin(), retried->end()) != "warm" || reads != 2) {
        throw std::runtime_error("cache retry did not publish the complete result");
    }
    reads = 0;
    const auto incomplete = Vulkan::ReadPipelineCacheSnapshot([&](size_t* size, void* data) {
        *size = 4;
        if (!data) return ReadResult::Complete;
        ++reads;
        return ReadResult::Retry;
    });
    if (incomplete || reads != 3) throw std::runtime_error("incomplete cache retries were not bounded");
    const std::array<u64, 3> code{1, 2, 3};
    const std::map<u64, u32> expected{{u64{1} << 32, 0}, {(u64{1} << 32) | 4, 123}};
    auto current = expected;
    const auto read = [&](u32 bank, u32 offset) -> std::optional<u32> {
        const auto found = current.find((u64{bank} << 32) | offset);
        return found == current.end() ? std::nullopt : std::optional{found->second};
    };
    const auto require = [](bool value) { if (!value) throw std::runtime_error("static match regression"); };
    require(Vulkan::MatchesStaticCompute(code, code, expected, read));
    current[(u64{1} << 32) | 4] = 124;
    require(!Vulkan::MatchesStaticCompute(code, code, expected, read));
    current = expected;
    current.erase(u64{1} << 32); // disabled/unmapped constant buffer
    require(!Vulkan::MatchesStaticCompute(code, code, expected, read));
    current = expected;
    auto changed = code;
    changed[2] = 4; // even a matching external hash cannot replace this comparison
    require(!Vulkan::MatchesStaticCompute(code, changed, expected, read));
    require(!Vulkan::MatchesStaticCompute(code, std::span{code}.first(2), expected, read));
    require(Vulkan::MatchesStaticCompute(code, code, {}, read));
    std::cout << "Static compute code and per-dispatch constant guards passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
