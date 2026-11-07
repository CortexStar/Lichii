# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(shader_sweep LANGUAGES C CXX)
    include(CTest)
    get_filename_component(REPOSITORY_ROOT "../.." ABSOLUTE)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    list(APPEND CMAKE_MODULE_PATH "${REPOSITORY_ROOT}/CMakeModules" "${REPOSITORY_ROOT}/externals/cmake-modules")
    include(DetectArchitecture)
    include(FetchContent)
    find_package(Git REQUIRED)
    find_package(Threads REQUIRED)
    find_package(Boost REQUIRED COMPONENTS filesystem context)
    find_package(OpenSSL REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)
    add_library(httplib::httplib ALIAS PkgConfig::HTTPLIB)
    pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
    add_library(lz4::lz4 ALIAS PkgConfig::LZ4)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    add_library(zstd::zstd ALIAS PkgConfig::ZSTD)
    add_library(stb::headers INTERFACE IMPORTED)
    set_target_properties(stb::headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${REPOSITORY_ROOT}/externals/stb")
    if(ARCHITECTURE_x86_64)
        find_package(xbyak CONFIG REQUIRED)
    endif()
    FetchContent_Declare(fmt URL https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
        URL_HASH SHA512=f0da82c545b01692e9fd30fdfb613dbb8dd9716983dcd0ff19ac2a8d36f74beb5540ef38072fdecc1e34191b3682a8542ecbf3a61ef287dbba0a2679d4e023f2)
    FetchContent_Declare(SPIRV-Headers OVERRIDE_FIND_PACKAGE URL https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
        URL_HASH SHA512=d624371dd455c66a300344c89812598ffe11b5eedba555779f789e85c29dc67317741858c60e0744a1e6755cc0d2759b8659f0674f4cc31479c4cb6fc25ed23b)
    set(SIRIT_USE_SYSTEM_SPIRV_HEADERS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(sirit URL https://github.com/eden-emulator/sirit/archive/refs/tags/v1.0.7.tar.gz
        URL_HASH SHA512=92512b9c100b9cbf98ab658eeaf04ea0fbe4a99b79cff1fabc220320b008fdf91281476bdaa835a4d2875b7826318cf7d4da293a18b6afc6845f3db969af4e3a
        PATCH_COMMAND ${GIT_EXECUTABLE} apply ${REPOSITORY_ROOT}/.patch/sirit/0001-array-length.patch)
    FetchContent_Declare(zbic URL https://github.com/kinnay/zbic/archive/11b08f2712264bbed731545085cbd9702096ceb7.tar.gz
        URL_HASH SHA512=fbe2f37986377d7f0d96ae3224c80b5971df6e8b6961f68061f1975ebb2e8cb78f07b90f014b007be4023dbf5513581504e7f7d61a5237cb2a8a7a61ae11e482)
    FetchContent_MakeAvailable(fmt SPIRV-Headers sirit zbic)
    set(ZBIC_INCLUDE_DIR "${zbic_SOURCE_DIR}/src")
    include_directories("${REPOSITORY_ROOT}/src" "${REPOSITORY_ROOT}/externals")
    add_compile_definitions(NDEBUG)
    function(create_target_directory_groups target)
    endfunction()
    add_subdirectory("${REPOSITORY_ROOT}/src/common" common)
    add_subdirectory("${REPOSITORY_ROOT}/src/shader_recompiler" shader_recompiler)
else()
    set(REPOSITORY_ROOT "${CMAKE_SOURCE_DIR}")
endif()

add_executable(shader-sweep main.cpp diagnostics.cpp)
target_link_libraries(shader-sweep PRIVATE common shader_recompiler)
target_include_directories(shader-sweep PRIVATE ${REPOSITORY_ROOT}/src)
create_target_directory_groups(shader-sweep)
add_executable(pipeline-key-test test_pipeline_key.cpp)
target_link_libraries(pipeline-key-test PRIVATE common)
target_include_directories(pipeline-key-test PRIVATE ${REPOSITORY_ROOT}/src)
if(YUZU_TESTS OR BUILD_TESTING)
    add_test(NAME pipeline-key COMMAND pipeline-key-test)
endif()

add_executable(static-compute-match-test test_static_compute_match.cpp)
target_include_directories(static-compute-match-test PRIVATE ${REPOSITORY_ROOT}/src)

add_executable(pipeline-cache-certificate-test test_pipeline_cache_certificate.cpp
    ${REPOSITORY_ROOT}/src/video_core/renderer_vulkan/pipeline_cache_certificate.cpp)
target_link_libraries(pipeline-cache-certificate-test PRIVATE common)
target_include_directories(pipeline-cache-certificate-test PRIVATE ${REPOSITORY_ROOT}/src)
if(YUZU_TESTS OR BUILD_TESTING)
    add_test(NAME pipeline-cache-certificate COMMAND pipeline-cache-certificate-test)
endif()

add_executable(static-buffer-bounds-test test_static_bounds.cpp)
target_link_libraries(static-buffer-bounds-test PRIVATE common shader_recompiler)
target_include_directories(static-buffer-bounds-test PRIVATE ${REPOSITORY_ROOT}/src)
if(YUZU_TESTS OR BUILD_TESTING)
    add_test(NAME static-compute-match COMMAND static-compute-match-test)
    add_test(NAME static-buffer-bounds
        COMMAND static-buffer-bounds-test "${CMAKE_CURRENT_BINARY_DIR}/static-buffer-bounds")
endif()
if(TARGET Vulkan::Headers)
    find_library(STATIC_BOUNDS_VULKAN_LOADER NAMES vulkan vulkan-1 HINTS "$ENV{VULKAN_SDK}/Lib")
    if(STATIC_BOUNDS_VULKAN_LOADER)
        add_executable(static-buffer-bounds-gpu-test test_static_bounds_gpu.cpp)
        target_link_libraries(static-buffer-bounds-gpu-test PRIVATE Vulkan::Headers ${STATIC_BOUNDS_VULKAN_LOADER})
        add_executable(pipeline-prewarm-test test_pipeline_prewarm.cpp)
        target_include_directories(pipeline-prewarm-test PRIVATE "${REPOSITORY_ROOT}/src")
        target_link_libraries(pipeline-prewarm-test PRIVATE common Vulkan::Headers ${STATIC_BOUNDS_VULKAN_LOADER})
    endif()
endif()

add_executable(archive-vertex-layout-test test_archive_vertex_layout.cpp)
target_link_libraries(archive-vertex-layout-test PRIVATE shader_recompiler)
target_include_directories(archive-vertex-layout-test PRIVATE ${REPOSITORY_ROOT}/src)
if(YUZU_TESTS OR BUILD_TESTING)
    add_test(NAME archive-vertex-layout COMMAND archive-vertex-layout-test)
endif()

add_executable(native-shader-archive-test test_archive.cpp
    ${REPOSITORY_ROOT}/src/shader_recompiler/frontend/ampere/shader_archive.cpp)
target_include_directories(native-shader-archive-test PRIVATE ${REPOSITORY_ROOT}/src)
if(YUZU_TESTS OR BUILD_TESTING)
    add_test(NAME native-shader-archive COMMAND native-shader-archive-test)
endif()

set(nup_reader_sources
    ${REPOSITORY_ROOT}/src/core/file_sys/nup_container.cpp
    ${REPOSITORY_ROOT}/src/core/file_sys/vfs/vfs.cpp
    ${REPOSITORY_ROOT}/src/core/file_sys/vfs/vfs_real.cpp
    ${REPOSITORY_ROOT}/src/core/file_sys/vfs/vfs_vector.cpp
    ${REPOSITORY_ROOT}/src/core/file_sys/vfs/vfs_offset.cpp)
add_executable(nup-shader-export nup_export.cpp ${nup_reader_sources})
target_link_libraries(nup-shader-export PRIVATE common OpenSSL::Crypto zstd::zstd)
target_include_directories(nup-shader-export PRIVATE ${REPOSITORY_ROOT}/src)
create_target_directory_groups(nup-shader-export)

add_library(nup-shader-support STATIC ${nup_reader_sources}
    ${REPOSITORY_ROOT}/src/video_core/nup_shader_cache.cpp)
target_link_libraries(nup-shader-support PUBLIC common shader_recompiler OpenSSL::Crypto zstd::zstd)
target_include_directories(nup-shader-support PUBLIC ${REPOSITORY_ROOT}/src)
add_executable(nup-shader-precompile precompile_nup.cpp)
target_link_libraries(nup-shader-precompile PRIVATE nup-shader-support)
create_target_directory_groups(nup-shader-precompile)
add_executable(nup-shader-lookup-test test_nup_lookup.cpp)
target_link_libraries(nup-shader-lookup-test PRIVATE nup-shader-support)
add_executable(nup-driver-cache-test test_nup_driver_cache.cpp)
target_link_libraries(nup-driver-cache-test PRIVATE nup-shader-support)

if(YUZU_TESTS OR BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME nup-shader-precompile
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_nup_precompile.py"
                $<TARGET_FILE:nup-shader-precompile>
                --lookup-test $<TARGET_FILE:nup-shader-lookup-test>
                --driver-cache-test $<TARGET_FILE:nup-driver-cache-test>
                --vertex-layout-test $<TARGET_FILE:archive-vertex-layout-test>)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME nup-shader-export-cli
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_nup_export.py"
                    $<TARGET_FILE:nup-shader-export>)
        add_test(NAME shader-sweep-cli
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py" $<TARGET_FILE:shader-sweep>)
        find_program(SPIRV_VAL_EXECUTABLE spirv-val REQUIRED)
        add_test(NAME shader-sweep-captured
            COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/test_captured.py"
                    $<TARGET_FILE:shader-sweep> --spirv-val "${SPIRV_VAL_EXECUTABLE}")
        add_test(NAME shader-sweep-cache-parser
            COMMAND Python3::Interpreter -m unittest discover
                    -s "${REPOSITORY_ROOT}/tools/nx2" -p "test_shader_*.py")
    endif()
endif()
