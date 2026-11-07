// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <string_view>
#include <unordered_map>

#include <boost/container/static_vector.hpp>

#include "common/common_types.h"
#include "common/dkb_cpu_trace.h"
#include "common/dkb_frame_trace.h"
#include "video_core/buffer_cache/nx2_write_handoff.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/hazard_tracker.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/oot_network.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/dynamic_state_delta.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_fence_manager.h"
#include "video_core/renderer_vulkan/vk_frame_dump.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {

namespace Engines {
class Maxwell3D;
}

} // namespace Tegra

namespace Vulkan {

class OotObjectMotion;

struct FramebufferTextureInfo;

class StateTracker;

class AccelerateDMA : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache,
                           Scheduler& scheduler);

    bool BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) override;

    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;

    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override;

    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override;

private:
    template <bool IS_IMAGE_UPLOAD>
    bool DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                            const Tegra::DMA::BufferOperand& src,
                            const Tegra::DMA::ImageOperand& dst);

    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    Scheduler& scheduler;
};

class RasterizerVulkan final : public VideoCore::RasterizerInterface,
                               protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                              Tegra::MaxwellDeviceMemoryManager& device_memory_,
                              const Device& device_, MemoryAllocator& memory_allocator_,
                              StateTracker& state_tracker_, Scheduler& scheduler_);
    ~RasterizerVulkan() override;

    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    GpuWrittenList ReadGpuWrittenCommandList(DAddr addr, std::span<u8> out, bool past_sync,
                                             u64* pending_tick = nullptr) override;
    bool IsGpuTickDone(u64 tick) override;
    void NoteGpuWrittenCommandListSynced(DAddr page) override;
    void FlushGpuWrittenCommandListPages(std::span<const DAddr> pages) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    bool QueueFlushRegion(DAddr addr, u64 size) override;
    bool HasPendingQueries(DAddr addr, u64 size) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void PrepareRegionWrite(DAddr addr, u64 size, VideoCommon::CacheType which) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(DAddr addr, u64 size) override;
    void OnCacheInvalidations(std::span<const std::pair<DAddr, u64>> ranges) override;
    bool TryCacheInvalidation(DAddr addr, u64 size) override;
    bool OnCPUWrite(DAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force = true) override;
    bool FencesQuiet(u64& epoch) override;
    u64 FenceWorkEpoch() const override;
    void FlushQueryResults(DAddr addr, u64 size) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    bool AccelerateConditionalRendering() override;
    bool HasDrawTransformFeedback() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    bool SupportsGameDlssNetwork() const override;
    bool UsesNvidiaDlss() const override;
    bool CanProbeGameDlssOutput() const override {
        return true;
    }
    std::optional<VideoCore::GameDlssProbe> ProbeGameDlssOutput(
        const Tegra::Engines::Fermi2D::Surface& src,
        const Tegra::Engines::Fermi2D::Surface& dst) override;
    bool AccelerateTemporalUpscale(const Tegra::Engines::Fermi2D::Surface& src,
                                   const Tegra::Engines::Fermi2D::Surface& dst,
                                   const Tegra::Engines::Fermi2D::Surface& motion_vectors,
                                   const Tegra::Texture::TICEntry* depth,
                                   std::array<f32, 2> jitter,
                                   std::array<f32, 2> motion_scale, bool use_history = true) override;
    VideoCommon::OotNetwork::BypassState* OotNetworkBypass() override;
    void OotSceneEnd() override;
    bool AccelerateOotDlss(const Tegra::Engines::Fermi2D::Surface& src,
                           const Tegra::Engines::Fermi2D::Surface& dst,
                           const Tegra::Texture::TICEntry& depth, std::array<f32, 2> jitter,
                           const std::array<f32, 16>& reproject, bool flip_y,
                           bool reset) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;

    void InitializeChannel(Tegra::Control::ChannelState& channel) override;

    void BindChannel(Tegra::Control::ChannelState& channel) override;

    void ReleaseChannel(s32 channel_id) override;
    std::optional<FramebufferTextureInfo> AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                                            VAddr framebuffer_addr,
                                                            u32 pixel_stride);

private:
    static constexpr const u64 NEEDS_D24[] = {
        0x01006A800016E000ULL, // SSBU
        0x0100E95004038000ULL, // XC2
        0x0100A6301214E000ULL, // FE:Engage
    };
    static constexpr size_t MAX_TEXTURES = 192;
    static constexpr size_t MAX_IMAGES = 48;
    static constexpr size_t MAX_IMAGE_VIEWS = MAX_TEXTURES + MAX_IMAGES;

    static constexpr VkDeviceSize DEFAULT_BUFFER_SIZE = 4 * sizeof(float);

    template <typename Func>
    void PrepareDraw(bool is_indexed, Func&&, bool is_indirect = false);

    void CaptureIndirectTrace();
    void CollectIndirectTrace();
    void TraceDrawFailure(std::string_view reason);
    /// Remembers render target 0 of a draw with one of Ocarina of Time's tonemap shaders.
    void TrackOotTonemappedImage(const GraphicsPipeline& pipeline);
    bool TryNx2IndirectReset(bool is_indexed, u32 instance_count);

    void FlushWork();

    void TraceRenderState(bool is_indexed, const char* outcome, const Framebuffer* framebuffer);

    /// Records the copies of the command list pages the work just recorded wrote and submits, so
    /// the GPU makes them now (BufferCache::Nx2CopyListPages).
    void SubmitNx2ListCopies();

    /// Records the dynamic state of the draw being prepared with `pipeline` (null for
    /// DrawTexture's blit).
    void UpdateDynamicStates(const GraphicsPipeline* pipeline);

    void HandleTransformFeedback();

    /// The value a dynamic state slot takes for the registers and the draw's pipeline (null:
    /// DrawTexture); 0 words when the device doesn't record it dynamically.
    u32 ComputeDynamicState(DynamicStateDelta::Slot slot,
                            const Tegra::Engines::Maxwell3D::Regs& regs,
                            const GraphicsPipeline* pipeline, u32* out);
    /// Stages the slot's value; without batching records it at once.
    void StageDynamicState(DynamicStateDelta::Slot slot,
                           const Tegra::Engines::Maxwell3D::Regs& regs,
                           const GraphicsPipeline* pipeline);
    /// Records the staged slots among `slots` as one command.
    void RecordDynamicStatePacket(u64 slots);
    /// EDEN_NX2_NATIVE_VERIFY=dynstate: every slot the draw uses holds what the registers give.
    void VerifyDynamicState(const Tegra::Engines::Maxwell3D::Regs& regs,
                            const GraphicsPipeline* pipeline, bool emulated_mesh);
    /// Ocarina of Time DLSS mode: this frame's sub-pixel jitter (render pixels) for draws into
    /// the main scene depth, or zero for any other draw.
    std::array<float, 2> OotDlssViewportJitter(const Tegra::Engines::Maxwell3D::Regs& regs) const;
    std::array<float, 2> applied_viewport_jitter{};
    /// The viewports are staged: the jitter they now carry.
    void NoteOotViewportJitter(const Tegra::Engines::Maxwell3D::Regs& regs);
    /// Ocarina of Time DLSS mode: this frame's camera reprojection, from the first scene draw.
    void CaptureOotCamera();
    /// Ocarina of Time DLSS on the HDR scene colour, right before the tonemap draw (HdrInput).
    void OotDlssBeforeTonemap();
    /// Ocarina of Time's half-resolution effects layer (fireflies, fairies, dust), learned from
    /// the draws into it: DLSS's bias-to-current-colour mask.
    std::optional<Tegra::Engines::Maxwell3D::Regs::RenderTargetConfig> oot_effects_rt;
    /// Its volumetric fog layer (the second of two RGBA16F targets at half size).
    std::optional<Tegra::Engines::Maxwell3D::Regs::RenderTargetConfig> oot_fog_rt;
    /// The scene frame whose fog composite was just drawn: DLSS (or the copy before the
    /// particles) runs at the next draw.
    u64 oot_dlss_after_fog_frame = ~0ULL;
    std::string oot_post_fog_order; ///< the draws after it, for a log line
    bool oot_post_fog_recording{};
    /// Copies the scene colour before the particles (0) or at the tonemap (1), or with
    /// `restore` back.
    bool OotSnapshotScene(u32 which, bool restore = false);
    /// After the tonemap draw (DLSS after it): keeps its image with the particles, then draws it
    /// again from the scene colour before them.
    void OotRedrawTonemap(bool is_indexed, u32 instance_count);
    bool oot_redraw_tonemap{};
    bool oot_redrawing{};
    /// Pipelines already checked for scene draws without object motion.
    std::unordered_set<const GraphicsPipeline*> oot_reject_seen;
    /// Ocarina of Time object motion: creates its resources before a motion pipeline binds them.
    void PrepareOotObjectMotion();
    /// The motion image's size: the scene depth's, scaled, or 1x1 until it is known.
    [[nodiscard]] VkExtent2D OotObjectMotionSize() const;
    /// Pushes the draw's history slots (or turns object motion off for it) before it is recorded.
    void PushOotObjectMotion(u32 base_vertex, u32 num_vertices, u32 num_instances,
                             u32 base_instance, bool is_indexed, bool enabled);
    /// Smallest and largest index of an index buffer range (restart index skipped), cached.
    [[nodiscard]] std::optional<std::pair<u32, u32>> OotIndexBounds();
    std::unique_ptr<OotObjectMotion> oot_object_motion;
    GraphicsPipeline* oot_motion_pipeline{};
    std::unordered_map<u64, std::pair<u32, u32>> oot_index_bounds;
    std::vector<u8> oot_index_scratch;
    u64 oot_jittered_updates{};
    /// Whether HostLogicOpEnable differs from what the command buffer holds (or it holds none).
    [[nodiscard]] bool LogicOpEnableChanged(const Tegra::Engines::Maxwell3D::Regs& regs) const;
    /// The viewports alone (clears).
    void UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs);

    void UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs);

    void BuildDynamicStateMask();

    using VertexInputLayout = DynamicStateDelta::VertexInputLayout;
    /// Attribute formats, per binding stride, instancing and divisor, and the repacked bindings.
    using VertexInputKey = std::array<u32, 32 + 32 * 3 + 1>;
    [[nodiscard]] VertexInputKey MakeVertexInputKey(
        const Tegra::Engines::Maxwell3D::Regs& regs) const;
    [[nodiscard]] u32 MaxVertexInputAttributes() const;
    [[nodiscard]] u32 MaxVertexInputBindings() const;
    struct VertexInputKeyHash {
        size_t operator()(const VertexInputKey& key) const noexcept;
    };
    void BuildVertexInputLayout(const Tegra::Engines::Maxwell3D::Regs& regs, u32 max_attributes,
                                u32 max_bindings, VertexInputLayout& layout) const;
    /// BufferCacheRuntime::SetVertexRepackLayout, skipped while its inputs are unchanged.
    u32 UpdateVertexRepackLayout(bool dynamic_input);

    void FrameDumpPass(const Framebuffer* framebuffer);
    /// EDEN_NX2_PENDING_WRITE_TRACE: logs bound buffer ranges with collected, unapplied CPU writes.
    void TracePendingCpuWrites(bool compute, std::span<const u64> shader_hashes);
    bool DrawDebugHooks(bool is_indexed);
    void DrawDebugHooksConfigured(GraphicsPipeline* pipeline, bool is_indexed, bool plain_indirect);
    /// Records the uniform and storage buffers of the current draw or dispatch for the frame dump.
    bool FrameDumpBuffers(bool compute, std::span<const u64> shader_hashes);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;

    const Device& device;
    MemoryAllocator& memory_allocator;
    StateTracker& state_tracker;
    Scheduler& scheduler;

    /// Which guest dispatches need the barrier before them.
    VideoCommon::HazardTracker dispatch_hazards;
    VideoCommon::HazardTracker::Accesses dispatch_accesses;
    /// Scheduler::RecordedCommands after the last dispatch's own commands.
    u64 dispatch_recorded_commands = ~u64{0};

    StagingBufferPool staging_pool;
    DescriptorPool descriptor_pool;
    GuestDescriptorQueue guest_descriptor_queue;
    ComputePassDescriptorQueue compute_pass_descriptor_queue;
    DescriptorBufferRing descriptor_buffer_ring;
    BlitImageHelper blit_image;
    RenderPassCache render_pass_cache;

    TextureCacheRuntime texture_cache_runtime;
    TextureCache texture_cache;
    BufferCacheRuntime buffer_cache_runtime;
    BufferCache buffer_cache;
    VideoCommon::Nx2WriteHandoff write_handoff;
    QueryCacheRuntime query_cache_runtime;
    QueryCache query_cache;
    PipelineCache pipeline_cache;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;
    FrameDump frame_dump;

    boost::container::static_vector<u32, MAX_IMAGE_VIEWS> image_view_indices;
    std::array<VideoCommon::ImageViewId, MAX_IMAGE_VIEWS> image_view_ids;
    boost::container::static_vector<VkSampler, MAX_TEXTURES> sampler_handles;

    u32 draw_counter = 0;
    u64 nx2_dispatch_serial = 0;     ///< Compute dispatches recorded (WaitForIdle)
    u64 nx2_wfi_dispatch_serial = 0; ///< nx2_dispatch_serial at the last real wait for idle
    u64 render_trace_frame = 0;
    /// Frames for the draw input watch's re-arm delays, and its last report.
    u64 draw_watch_frame = 0;
    std::chrono::steady_clock::time_point draw_watch_report = std::chrono::steady_clock::now();
    u32 render_trace_draws = 0;
    /// Any of VideoCommon::DrawDebugSwitches is set: run the per-draw debug hooks.
    const bool draw_debug_hooks;
    u32 render_trace_clears = 0;
    bool pending_native_work{};
    bool mesh_draw{}; ///< the draw being prepared uses a task/mesh pipeline
    /// Fragment output components the dynamic color write mask was last recorded with
    u32 last_color_output_mask{};
    /// The pipeline alpha to coverage/one were last staged with
    const GraphicsPipeline* last_dyn_pipeline{};
    /// Render target formats the blend enables were last staged with
    std::array<Tegra::RenderTargetFormat, Tegra::Engines::Maxwell3D::Regs::NumRenderTargets>
        last_rt_formats{};
    VideoCommon::OotNetwork::BypassState oot_network_bypass;
    u32 mesh_emulation_groups{};             ///< mesh workgroups of the emulated draw
    u32 mesh_emulation_vertices_per_group{}; ///< 0 unless the draw is an emulated mesh draw
    /// Mesh emulation diagnostics with a task program: one draw's distribution arguments.
    struct {
        std::span<const u8> args;
        u64 tick{};
        u32 tasks{};
        u32 capacity{};
        bool pending{};
    } mesh_task_probe;
    /// With a task program: the vkCmdDrawIndirect arguments the distribution pass writes.
    struct {
        VkBuffer buffer{};
        VkDeviceSize offset{};
    } mesh_emulation_indirect;
    /// Mesh emulation diagnostics: one draw's output, read back once the GPU has written it.
    struct MeshEmulationProbe {
        std::span<const u8> data;
        u64 tick{};
        u64 frame{};
        u32 groups{};
        u32 block_words{};
        u32 vertex_lines{};
        u32 max_vertices{};
        u32 max_primitives{};
        std::array<u32, 4> position_lines{};
        bool pending{};
    } mesh_probe;
    void ReadMeshEmulationProbe();
    /// Mesh emulation with a task program: the output arena every task draw of a submission
    /// allocates its mesh workgroups from, and its host-visible counter of allocated words.
    struct MeshArena {
        vk::Buffer buffer;
        vk::Buffer counter;
        u64 size{};
        u64 tick{};
        bool counted{}; ///< the counter of its last submission was read
    };
    std::vector<MeshArena> mesh_arenas;
    size_t mesh_arena_current{~size_t{}};
    u64 mesh_arena_size{64ULL << 20};
    /// The current submission's arena, after its counter is reset.
    const MeshArena& AcquireMeshArena();

    /// What the command buffer holds of each dynamic state, and the slots staged for the draw.
    DynamicStateDelta::Tracker dyn_state;
    /// The slots the draws record dynamically on this device (BuildDynamicStateMask).
    u64 dyn_state_slots{};
    DynamicStateDelta::ApplyOptions dyn_apply_options{};
    /// One command per draw for the changed states (false: one per changed state).
    bool dyn_state_batch{true};
    /// The dirty flags UpdateDynamicStates consumes on this device: none set, nothing to record.
    Tegra::Engines::Maxwell3D::DirtyState::Flags dyn_state_mask{};
    bool dyn_state_mask_built{};
    /// AMD with a dynamic logic op enable: VideoCommon::HostLogicOpEnable's float workaround.
    bool logic_op_float_workaround{};

    /// Vertex input layouts, built once and recorded by pointer
    /// (immutable while the rasterizer lives).
    std::unordered_map<VertexInputKey, std::unique_ptr<VertexInputLayout>, VertexInputKeyHash>
        vertex_input_layouts;
    VertexInputKey last_vertex_input_key{};
    const VertexInputLayout* last_vertex_input_layout{};
    /// Attribute formats and strides SetVertexRepackLayout last chose its repack mask from.
    std::array<u32, 64> repack_key{};
    bool repack_key_valid{};
    bool repack_key_dynamic{};

    struct IndirectTrace {
        vk::Buffer readback;
        u64 tick{};
        u64 frame{};
        GPUVAddr address{};
        GPUVAddr count_address{};
        u32 max_draws{};
        u32 sampled_commands{};
        bool pending{};
        bool include_count{};
        bool byte_count{};
    };
    std::array<IndirectTrace, 8> indirect_traces;
    Common::DkbFrameTrace::RateLimit trace_rate;
    Common::DkbFrameTrace::RateLimit trace_cpu_rate;
    std::optional<Common::DkbCpuTrace::Snapshot> trace_cpu_snapshot;
    u64 trace_cpu_frame{};
    bool trace_sample_frame{true};
    u64 trace_frame{};
    u64 trace_window_frames{};
    u64 trace_draws{};
    u64 trace_skipped{};
    u64 trace_indirect{};
    u64 trace_zero_direct{};
    /// Vertex and fragment hashes of the draw being recorded, for GPU timing.
    std::pair<u64, u64> trace_draw_hashes{};
    u32 trace_captures{};
    u32 trace_capture_dropped{};
    u32 trace_failure_details{};
};

} // namespace Vulkan
