# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    video_core/ampere_fast_fusion.cpp
    video_core/ampere_async_copy.cpp
    video_core/ampere_trap_guards.cpp
    video_core/async_draw_policy.cpp
    video_core/multi_range_refresh.cpp
    video_core/command_fetch_order.cpp
    hid_core/sensor_info.cpp
    common/paged_scalar.cpp
    common/queue_lifetime.cpp
    common/queue_timed_wait.cpp
    common/range_set_move.cpp
    video_core/uniform_cache_history.cpp
    video_core/x8d24_conversion.cpp
    video_core/ampere_ported_half_compare.cpp
    video_core/ampere_geometry_capture.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/ampere_ported_conversions.cpp
    video_core/ampere_ported_uniform_vote.cpp
    video_core/ampere_ported_textures.cpp
    video_core/ampere_tennis_i2f.cpp
    video_core/pipeline_cache_files.cpp
    video_core/storage_binding_capacity.cpp
    video_core/native_indirect_reset.cpp
    video_core/ampere_matrix.cpp
    video_core/spirv_software_matrix.cpp
    video_core/spirv_builtin_capabilities.cpp
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/thread_startup_policy.cpp
    core/nup_writer.cpp
    core/internal_network/network.cpp
    video_core/memory_tracker.cpp
    video_core/vertex_repack.cpp
    video_core/ampere_returns.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
