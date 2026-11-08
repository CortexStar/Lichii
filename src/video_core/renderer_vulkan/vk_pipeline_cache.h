// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <thread>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_prebuild.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"
#include "video_core/nx2_ir_cache.h" // nx2 shaderwarm

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

struct ComputePipelineCacheEntry {
    std::unique_ptr<ComputePipeline> pipeline;
    std::string failure;
    /// Constant buffer slots that were null pointers at translation (see NullStoragePointerSlots)
    std::vector<std::pair<u32, u32>> null_pointer_slots;
    /// Constant buffer slots bound as storage buffer pointers (see MappedStoragePointerKeys)
    std::vector<u64> mapped_pointers;
    u32 rebuilds{};

    [[nodiscard]] ComputePipeline* Get(bool native, GPUVAddr address,
                                       const ComputePipelineCacheKey& key) const;
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    void TickFrame();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline(u64& native_key);

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    /// Returns `pipeline` when it is built or its draw must wait for it, nullptr when the draw
    /// may be skipped while it builds (asynchronous shaders, CanDiscardPendingDraw).
    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) noexcept;

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools = nullptr,
        std::unique_ptr<EmissionBudget::Reservation> reservation = nullptr);

    [[nodiscard]] bool StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const;

    /// Translates the current compute program, binding the pointers in keep_pointers whatever
    /// their slots hold now (see GenericEnvironment::KeepStoragePointers)
    ComputePipelineCacheEntry CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                    const ShaderInfo* shader,
                                                    std::span<const u64> keep_pointers = {});

    ComputePipelineCacheEntry CreateComputePipeline(ShaderPools& pools,
                                                     const ComputePipelineCacheKey& key,
                                                     Shader::Environment& env,
                                                     PipelineStatistics* statistics,
                                                     bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    /// Queues one write of the driver pipeline cache file on the serialization thread, with no
    /// throttle. For the end of a precache: its builds never reach the timed flush, which counts
    /// only pipelines built during play.
    void QueueVulkanPipelineCacheSave();

    /// One build of the shader precache: builds a pipeline and destroys it, true when it built.
    struct PrecacheJob {
        u64 graphics_hash{}; ///< The cached graphics pipeline it builds, 0 for a compute program
        Common::UniqueFunction<bool> build;
    };

    /// Shader precache before the game starts (precache_shaders), on every CPU thread: the cached
    /// graphics pipelines this boot skipped (`jobs`; `precached` holds those built by earlier
    /// precaches) and the compute programs of the title's precache file (made from the game files,
    /// missing reads guessed) are built, then thrown away. They only fill the driver's shader
    /// cache, so a later build of the same pipeline finds the driver's work done. Reports progress
    /// through `callback` and returns when it is done, skipped from the loading screen or the load
    /// is stopped.
    void PrecacheShaders(u64 title_id, std::vector<PrecacheJob> jobs,
                         std::unordered_set<u64> precached, std::stop_token stop_loading,
                         const VideoCore::DiskResourceLoadCallback& callback);

    // ---- nx2 shaderwarm (docs/shader-precompile.md; defined in vk_shaderwarm.cpp) ---------------
    /// The stage's translation from the per-stage cache, when it has one whose environment
    /// inputs match env (and adopts its answers into env). False: translate it.
    bool TryCachedTranslation(ShaderPools& pools, u64 unique_hash, Shader::Environment& env,
                              Shader::IR::Program& out);
    /// EDEN_NX2_IR_LEARN=1: whether live translations are stored as per-stage cache records, and
    /// arming env's query log before such a translation (no-op when off or env can't be stored).
    [[nodiscard]] bool Nx2LearnArm(Shader::Environment& env) const;
    /// The per-stage cache key of a graphics pipeline's stage (task/mesh: the program's own hash).
    [[nodiscard]] u64 Nx2StageRecordHash(const GraphicsPipelineCacheKey& key, size_t index,
                                         Shader::Environment& env) const;
    /// Stores a live translation (made with the query log armed, right after TranslateProgram) as
    /// a record under record_hash, so the next session uses it instead of translating again.
    void Nx2LearnTranslation(u64 record_hash, Shader::Environment& env,
                             const Shader::IR::Program& program);
    /// EDEN_NX2_PRECOMPILE=<pack>: translates every stage program of the pack into the per-stage
    /// cache on background threads.
    void RunPrecompile(std::filesystem::path pack_path, std::stop_token stop);
    /// Opens <title>/vulkan_ir.bin when it exists (or EDEN_NX2_PRECOMPILE makes it) and starts the
    /// precompile.
    void Nx2OpenStageCache(const std::filesystem::path& base_dir);
    void Nx2StopPrecompile();
    /// Whether a graphics pipeline's stage takes part in the per-stage translation cache, and its
    /// cached translation when a record's inputs match env.
    [[nodiscard]] bool Nx2CacheableStage(size_t index, Shader::Environment& env,
                                         bool uses_vertex_a) const;
    bool Nx2TryCachedStage(ShaderPools& pools, const GraphicsPipelineCacheKey& key, size_t index,
                           Shader::Environment& env, Shader::IR::Program& out);
    /// EDEN_NX2_PIPELINE_STATS: a native VS+PS pipeline left monolithic in library mode.
    void Nx2CountMonolithic(const GraphicsPipelineCacheKey& key, const char* reason) const;
    /// EDEN_NX2_GPL: the optimized-link workers.
    void Nx2InitGpl();
    /// EDEN_NX2_GPL_CHECK=1: logs the stage's SPIR-V inputs that differ from the precompile's;
    /// EDEN_NX2_GPL_DUMP=<dir>: writes the stage's library SPIR-V and state.
    void Nx2GplCheck(const GraphicsPipelineCacheKey& key, const Shader::IR::Program& program,
                     size_t index, std::span<const u32> spirv) const;
    /// EDEN_NX2_PIPELINE_STATS / EDEN_NX2_PIPELINE_LOG for a runtime graphics pipeline.
    void Nx2ReportGraphicsBuild(std::chrono::steady_clock::time_point begin,
                                std::chrono::steady_clock::duration env_time);
    void Nx2LogRuntimePipeline(const GraphicsEnvironments& environments,
                               const GraphicsPipeline* pipeline) const;
    /// The library path's state for a stage library made without a pipeline (precompile): the
    /// most common draw state, so the driver's cache key matches the first real use.
    [[nodiscard]] GraphicsPipelineCacheKey GplDefaultKey(Shader::Stage stage) const;
    /// The runtime info a library stage is emitted with (no previous stage; vertex attributes
    /// class-independent when the device allows it).
    [[nodiscard]] Shader::RuntimeInfo MakeGplRuntimeInfo(const Shader::IR::Program& program,
                                                         const GraphicsPipelineCacheKey& key) const;
    /// The stage's library SPIR-V (vertex set 0 / fragment set 1, own bindings, unpruned inputs).
    [[nodiscard]] std::vector<u32> EmitGplStage(Shader::IR::Program& program,
                                                const GraphicsPipelineCacheKey& key) const;

    VideoCommon::Nx2IrCache::StageCache ir_cache;
    /// Learned records stored this session (hash of the record's checks), so one translation is
    /// stored once even when several pipelines translate the stage before the record is found.
    std::mutex learned_mutex;
    std::unordered_set<u64> learned_records;
    std::jthread precompile_thread;
    /// Graphics pipeline libraries (EDEN_NX2_GPL=1, vk_gpl.h); outlives the pipelines.
    GplLibraryCache gpl_libraries;
    /// Whether the last runtime compute pipeline came from the translation cache (GPU thread).
    bool last_compute_from_ir{};
    // ---- end nx2 shaderwarm ---------------------------------------------------------------------

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    const size_t num_pipeline_workers;
    EmissionBudget emission_budget;

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    /// Wait budget of native draws for unbuilt pipelines left in this frame, in microseconds
    /// (native_async_draws = Bounded). Only touched on the GPU thread; reset in TickFrame.
    s64 native_wait_budget_us{8000};
    s64 native_wait_used_us{}; ///< Budget spent since the last 10 s log line.
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since

    ::Common::unordered_map<ComputePipelineCacheKey, ComputePipelineCacheEntry> compute_cache;
    /// Replaced compute pipelines, destroyed once the GPU and their queued build are done with
    /// them (tick, pipeline)
    std::deque<std::pair<u64, std::unique_ptr<ComputePipeline>>> retired_compute_pipelines;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;
    std::mutex spare_pools_mutex;
    std::vector<std::shared_ptr<ShaderPools>> spare_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    void SaveUsedPipelines();

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    /// Requested first when the cache is destroyed, to stop a rewrite of the disk cache: Android
    /// and yuzu_cmd load with a stop token that never stops
    std::stop_source serialization_stop;
    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;
    PipelinePrebuild<GraphicsPipelineCacheKey, GraphicsPipeline> background_pipelines;
    /// nx2 shaderwarm: optimized links of library pipelines (background); after graphics_cache:
    /// stops first.
    std::unique_ptr<Common::ThreadWorker> gpl_lto_workers;
};

} // namespace Vulkan
