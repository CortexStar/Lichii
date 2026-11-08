# Only interface declarations are used. NGX core comes from the installed NVIDIA driver;
# model libraries are obtained separately at runtime, never linked or packaged here.
set(LICHII_NGX_SDK "" CACHE PATH "NVIDIA NGX SDK root (empty: fetch pinned interface headers)")
if (LICHII_NGX_SDK)
    set(NGX_INCLUDE_DIR "${LICHII_NGX_SDK}/include")
else()
    set(NGX_INCLUDE_DIR "${CMAKE_BINARY_DIR}/externals/ngx/include")
    file(MAKE_DIRECTORY "${NGX_INCLUDE_DIR}")
    set(ngx_revision "9981c6350dc58c0ce21f024fd65e31e851c31689")
    set(ngx_files nvsdk_ngx_defs.h nvsdk_ngx_defs_vk.h nvsdk_ngx_params.h nvsdk_ngx_vk.h)
    set(ngx_hashes
        ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
        0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031
        943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
        2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d)
    foreach(name hash IN ZIP_LISTS ngx_files ngx_hashes)
        file(DOWNLOAD
            "https://raw.githubusercontent.com/NVIDIA/DLSS/${ngx_revision}/include/${name}"
            "${NGX_INCLUDE_DIR}/${name}"
            EXPECTED_HASH "SHA256=${hash}" TLS_VERIFY ON)
    endforeach()
endif()
foreach(name nvsdk_ngx_defs.h nvsdk_ngx_defs_vk.h nvsdk_ngx_params.h nvsdk_ngx_vk.h)
    if (NOT EXISTS "${NGX_INCLUDE_DIR}/${name}")
        message(FATAL_ERROR "Missing NVIDIA NGX interface header: ${NGX_INCLUDE_DIR}/${name}")
    endif()
endforeach()
