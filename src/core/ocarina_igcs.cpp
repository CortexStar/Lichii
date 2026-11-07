// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// IGCS camera tools interface for Ocarina of Time's photo mode. The IGCS Connector add-on for
// ReShade (Frans 'Otis_Inf' Bouma, https://github.com/FransBouma/IgcsConnector) looks for a module
// in the process that exports IGCS_StartScreenshotSession and its siblings, and drives the camera
// through them (IGCS depth of field walks the camera over a small disc and blends the frames).
// The camera tools call the add-on's connectFromCameraTools() when they start and write their
// camera data into the buffer getDataFromCameraToolsBuffer() returns.

#include <cstdint>
#include <cstring>

#include "common/logging.h"
#include "core/ocarina_photo_mode.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

extern "C" {
// 0 started, 1 camera not enabled (photo mode off), 3 a session is already active.
__declspec(dllexport) int IGCS_StartScreenshotSession(std::uint8_t type) {
    return Core::OcarinaPhotoMode::Igcs::StartSession(type);
}
__declspec(dllexport) void IGCS_MoveCameraPanorama(float step_angle) {
    Core::OcarinaPhotoMode::Igcs::MovePanorama(step_angle);
}
__declspec(dllexport) void IGCS_MoveCameraMultishot(float step_left_right, float step_up_down,
                                                    float fov_degrees, bool from_start_position) {
    Core::OcarinaPhotoMode::Igcs::MoveMultishot(step_left_right, step_up_down, fov_degrees,
                                                from_start_position);
}
__declspec(dllexport) void IGCS_EndScreenshotSession() {
    Core::OcarinaPhotoMode::Igcs::EndSession();
}
}
#endif

namespace Core::OcarinaPhotoMode::Igcs {

namespace {
#ifdef _WIN32
using GetBufferFunction = std::uint8_t* (*)();
std::uint8_t* camera_data_buffer = nullptr;
#endif

/// The add-on's CameraToolsData (CameraToolsData.h in IgcsConnector), 84 bytes, no padding.
#pragma pack(push, 1)
struct CameraToolsData {
    std::uint8_t camera_enabled;
    std::uint8_t camera_movement_locked;
    std::uint8_t reserved1;
    std::uint8_t reserved2;
    float fov_degrees;
    float coordinates[3];
    float look_quaternion[4];
    float up[3];
    float right[3];
    float forward[3];
    float pitch;
    float yaw;
    float roll;
};
#pragma pack(pop)
static_assert(sizeof(CameraToolsData) == 84);
} // Anonymous namespace

bool Connect() {
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool connected = false;
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        const auto connect = reinterpret_cast<bool (*)()>(
            reinterpret_cast<void*>(GetProcAddress(entry.hModule, "connectFromCameraTools")));
        if (connect == nullptr) {
            continue;
        }
        const auto get_buffer = reinterpret_cast<GetBufferFunction>(
            reinterpret_cast<void*>(GetProcAddress(entry.hModule, "getDataFromCameraToolsBuffer")));
        connected = connect();
        camera_data_buffer = connected && get_buffer != nullptr ? get_buffer() : nullptr;
        break;
    }
    CloseHandle(snapshot);
    LOG_INFO(Core, "Ocarina of Time photo mode: IGCS Connector {}",
             connected ? "connected (IGCS depth of field can drive the camera)" : "not loaded");
    return connected;
#else
    return false;
#endif
}

void WriteCameraData(const CameraData& data) {
#ifdef _WIN32
    if (camera_data_buffer == nullptr) {
        return;
    }
    CameraToolsData out{};
    out.camera_enabled = data.enabled ? 1 : 0;
    out.camera_movement_locked = data.session ? 1 : 0;
    out.fov_degrees = data.fov_degrees;
    std::memcpy(out.coordinates, data.eye.data(), sizeof(out.coordinates));
    std::memcpy(out.look_quaternion, data.quaternion.data(), sizeof(out.look_quaternion));
    std::memcpy(out.up, data.up.data(), sizeof(out.up));
    std::memcpy(out.right, data.right.data(), sizeof(out.right));
    std::memcpy(out.forward, data.forward.data(), sizeof(out.forward));
    out.pitch = data.pitch;
    out.yaw = data.yaw;
    out.roll = data.roll;
    std::memcpy(camera_data_buffer, &out, sizeof(out));
#else
    (void)data;
#endif
}

} // namespace Core::OcarinaPhotoMode::Igcs
