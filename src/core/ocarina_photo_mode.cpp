// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/ocarina_photo_mode.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>

#include "common/logging.h"
#include "common/settings_input.h"
#include "core/core.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "video_core/photo_mode_state.h"

namespace Core::OcarinaPhotoMode {
namespace {

using Vec3 = std::array<f32, 3>;
using Button = Settings::NativeButton::Values;

constexpr f32 LookSpeed = 1.8f;        ///< radians per second at full stick
constexpr f32 RollSpeed = 0.8f;        ///< radians per second
constexpr f32 MoveSpeed = 4.0f;        ///< game units (about a metre) per second
constexpr f32 MaxPitch = 1.5f;
constexpr f32 StickDeadzone = 0.12f;
constexpr f32 FovSpeed = 0.5f;         ///< radians per second
constexpr f32 MinFov = 0.087f;         ///< 5 degrees
constexpr f32 MaxFov = 2.094f;         ///< 120 degrees
constexpr f32 DefaultFov = 0.8726647f; ///< 50 degrees, the game's own
constexpr u32 NormalTimeScale = 0x3f800000; // 1.0f

std::atomic<bool> toggle_requested{};
std::atomic<bool> active{};
std::atomic<bool> capture_requested{};
std::atomic<bool> hotsample_requested{};
std::atomic<bool> hotsample_active{};
std::atomic<bool> ratio_cycle_requested{};
std::atomic<u32> ratio_index{};

/// An IGCS session (the IGCS Connector add-on drives the camera from ReShade's thread).
struct IgcsSession {
    bool active{};
    bool start_pending{};
    bool end_pending{};
    f32 left_right{};
    f32 up_down{};
    f32 fov_degrees{};
    f32 pan{};
    Vec3 start_eye{};
    Vec3 start_right{};
    Vec3 start_up{};
    f32 start_yaw{};
    f32 start_fov{};
};
std::mutex igcs_mutex;
IgcsSession igcs;

struct State {
    u64 base{};
    Core::HID::EmulatedController* controller{}; ///< in configuration mode while on: no game input
    std::array<f32, 9> start_camera{};            ///< eye, target, up when photo mode started
    Vec3 eye{};
    f32 yaw{};
    f32 pitch{};
    f32 roll{};
    f32 distance{};
    f32 fov{}; ///< vertical, radians
    f32 start_fov{};
    f32 aspect{};
    u32 saved_time_scale{NormalTimeScale};
    bool frozen{};
    bool hud_hidden{};
    bool haze_hidden{};
    std::array<bool, Settings::NativeButton::NumButtons> held{};
    std::chrono::steady_clock::time_point last{};
};
State state;

Vec3 Add(const Vec3& a, const Vec3& b, f32 scale) {
    return {a[0] + b[0] * scale, a[1] + b[1] * scale, a[2] + b[2] * scale};
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 Normalize(const Vec3& v) {
    const f32 length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return length > 1e-6f ? Vec3{v[0] / length, v[1] / length, v[2] / length} : v;
}

Vec3 Forward(f32 yaw, f32 pitch) {
    return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
}

/// Whether the built-in photo mode patch is in this game's main (and so the state words are ours).
bool PatchInPlace(Core::Memory::Memory& memory, u64 base) {
    return memory.IsValidVirtualAddressRange(base + SiteOffset, 4) &&
           memory.Read32(base + SiteOffset) == Word(SiteBranch) &&
           memory.Read32(base + CaveOffset) == Word({Cave[0], Cave[1], Cave[2], Cave[3]});
}

/// The game's time manager, or 0 before it exists.
u64 TimeManager(Core::Memory::Memory& memory, u64 base) {
    const u64 slot = memory.Read64(base + TimeManagerSlot);
    if (slot == 0 || !memory.IsValidVirtualAddressRange(slot, 8)) {
        return 0;
    }
    const u64 manager = memory.Read64(slot);
    if (manager == 0 || !memory.IsValidVirtualAddressRange(manager + GlobalTimeScaleOffset, 4)) {
        return 0;
    }
    return manager;
}

/// The world stops through the time manager's global scale. Link's hair and cap are a spring
/// simulation on a clock of their own: they settle about 30 seconds after the freeze.
void Freeze(Core::Memory::Memory& memory, bool freeze) {
    const u64 manager = TimeManager(memory, state.base);
    if (manager != 0) {
        const u64 scale = manager + GlobalTimeScaleOffset;
        if (freeze) {
            if (!state.frozen) {
                const u32 current = memory.Read32(scale);
                state.saved_time_scale = current == FrozenTimeScale ? NormalTimeScale : current;
            }
            memory.Write32(scale, FrozenTimeScale);
        } else if (state.frozen) {
            memory.Write32(scale, state.saved_time_scale);
        }
    }
    state.frozen = freeze;
}

void SetHudHidden(bool hidden) {
    state.hud_hidden = hidden;
    VideoCore::PhotoMode::SetHideHud(hidden);
}

/// Writes the free camera into the patch's state, which the cave copies over the game's camera.
void WriteCamera(Core::Memory::Memory& memory) {
    const Vec3 forward = Forward(state.yaw, state.pitch);
    const Vec3 right = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
    const Vec3 level_up = Cross(right, forward);
    const f32 c = std::cos(state.roll);
    const f32 s = std::sin(state.roll);
    const Vec3 up = Normalize({level_up[0] * c + right[0] * s, level_up[1] * c + right[1] * s,
                               level_up[2] * c + right[2] * s});
    const Vec3 target = Add(state.eye, forward, state.distance);
    const u64 address = state.base + StateOffset;
    const std::array<f32, 9> camera{state.eye[0], state.eye[1], state.eye[2], target[0], target[1],
                                    target[2],    up[0],        up[1],        up[2]};
    for (size_t i = 0; i < camera.size(); ++i) {
        memory.Write32(address + EyeOffset + i * 4, std::bit_cast<u32>(camera[i]));
    }
    const auto matrix = LookAt(state.eye, target, up);
    for (size_t i = 0; i < matrix.size(); ++i) {
        memory.Write32(address + ViewMatrixOffset + i * 4, std::bit_cast<u32>(matrix[i]));
    }
    const f32 half = state.fov * 0.5f;
    const f32 tangent = std::tan(half);
    const std::array<f32, 4> fov{state.fov, std::sin(half), std::cos(half), tangent};
    for (size_t i = 0; i < fov.size(); ++i) {
        memory.Write32(address + FovOffset + i * 4, std::bit_cast<u32>(fov[i]));
    }
    memory.Write32(address + ProjectionXOffset, std::bit_cast<u32>(1.0f / (tangent * state.aspect)));
    memory.Write32(address + ProjectionYOffset, std::bit_cast<u32>(1.0f / tangent));
    memory.Write32(address + FovEnabledOffset, 1);
}

void StartFrom(const std::array<f32, 9>& camera) {
    state.eye = {camera[0], camera[1], camera[2]};
    Vec3 direction{camera[3] - camera[0], camera[4] - camera[1], camera[5] - camera[2]};
    state.distance = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                               direction[2] * direction[2]);
    if (!(state.distance > 1e-3f)) {
        direction = {0.0f, 0.0f, 1.0f};
        state.distance = 4.0f;
    }
    direction = Normalize(direction);
    state.yaw = std::atan2(direction[0], direction[2]);
    state.pitch = std::clamp(std::asin(std::clamp(direction[1], -1.0f, 1.0f)), -MaxPitch, MaxPitch);
    state.roll = 0.0f;
}

Core::HID::EmulatedController* FirstController(Core::System& system) {
    auto& hid = system.HIDCore();
    return hid.GetEmulatedController(hid.GetFirstNpadId());
}

void RememberButtons(const Core::HID::EmulatedController* controller) {
    if (controller == nullptr) {
        state.held.fill(false);
        return;
    }
    const auto buttons = controller->GetButtonsValues();
    for (size_t i = 0; i < state.held.size() && i < buttons.size(); ++i) {
        state.held[i] = buttons[i].value;
    }
}

bool Enter(Core::System& system) {
    auto& memory = system.ApplicationMemory();
    const u64 address = state.base + StateOffset;
    const u64 camera = memory.Read64(address + CameraPointerOffset);
    if (camera == 0 || !memory.IsValidVirtualAddressRange(camera + CameraEyeOffset, 36)) {
        LOG_INFO(Core, "Ocarina of Time photo mode: no camera yet");
        return false;
    }
    for (size_t i = 0; i < state.start_camera.size(); ++i) {
        state.start_camera[i] =
            std::bit_cast<f32>(memory.Read32(camera + CameraEyeOffset + i * 4));
    }
    StartFrom(state.start_camera);
    const f32 fov = std::bit_cast<f32>(memory.Read32(camera + CameraFovOffset));
    const f32 aspect = std::bit_cast<f32>(memory.Read32(camera + CameraAspectOffset));
    state.start_fov = fov > MinFov && fov < MaxFov ? fov : DefaultFov;
    state.fov = state.start_fov;
    state.aspect = aspect > 0.5f && aspect < 4.0f ? aspect : 16.0f / 9.0f;

    Freeze(memory, true);
    state.controller = FirstController(system);
    if (state.controller != nullptr) {
        state.controller->EnableConfiguration();
    }
    RememberButtons(state.controller);
    SetHudHidden(true);
    WriteCamera(memory);
    memory.Write32(address + EnabledOffset, 1);
    state.last = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{igcs_mutex};
        igcs = {};
    }
    Igcs::Connect();
    active = true;
    LOG_INFO(Core, "Ocarina of Time photo mode on");
    return true;
}

void Leave(Core::System& system) {
    if (state.base != 0) {
        auto& memory = system.ApplicationMemory();
        memory.Write32(state.base + StateOffset + EnabledOffset, 0);
        memory.Write32(state.base + StateOffset + FovEnabledOffset, 0);
        Freeze(memory, false);
    }
    if (state.controller != nullptr) {
        state.controller->DisableConfiguration();
        state.controller = nullptr;
    }
    state.frozen = false;
    SetHudHidden(false);
    state.haze_hidden = false;
    VideoCore::PhotoMode::SetHideHaze(false);
    ratio_index = 0;
    hotsample_active = false;
    {
        std::scoped_lock lock{igcs_mutex};
        igcs = {};
    }
    Igcs::WriteCameraData({});
    active = false;
    LOG_INFO(Core, "Ocarina of Time photo mode off");
}

/// An IGCS session (depth of field, panorama) moves the camera from where it started; returns
/// whether one is on.
bool ApplyIgcs() {
    std::scoped_lock lock{igcs_mutex};
    if (igcs.start_pending) {
        const Vec3 forward = Forward(state.yaw, state.pitch);
        const Vec3 right = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
        igcs.start_pending = false;
        igcs.start_eye = state.eye;
        igcs.start_right = right;
        igcs.start_up = Cross(right, forward);
        igcs.start_yaw = state.yaw;
        igcs.start_fov = state.fov;
        LOG_INFO(Core, "Ocarina of Time photo mode: IGCS session started");
    }
    if (igcs.active) {
        state.eye =
            Add(Add(igcs.start_eye, igcs.start_right, igcs.left_right), igcs.start_up, igcs.up_down);
        state.yaw = igcs.start_yaw - igcs.pan;
        state.fov = igcs.fov_degrees > 0.0f
                        ? std::clamp(igcs.fov_degrees * 0.01745329f, MinFov, MaxFov)
                        : igcs.start_fov;
    }
    if (igcs.end_pending) {
        igcs.end_pending = false;
        state.eye = igcs.start_eye;
        state.yaw = igcs.start_yaw;
        state.fov = igcs.start_fov;
        LOG_INFO(Core, "Ocarina of Time photo mode: IGCS session ended");
    }
    return igcs.active;
}

/// The camera for the IGCS Connector add-on (its IGCS_ shader uniforms).
void PublishIgcsCamera(bool session) {
    const Vec3 forward = Forward(state.yaw, state.pitch);
    const Vec3 right = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
    const Vec3 up = Cross(right, forward);
    // Rotation (right, up, forward as columns) to a quaternion.
    const f32 trace = right[0] + up[1] + forward[2];
    std::array<f32, 4> quaternion{0.0f, 0.0f, 0.0f, 1.0f};
    if (trace > 0.0f) {
        const f32 root = std::sqrt(trace + 1.0f) * 2.0f;
        quaternion = {(up[2] - forward[1]) / root, (forward[0] - right[2]) / root,
                      (right[1] - up[0]) / root, 0.25f * root};
    }
    Igcs::WriteCameraData({.enabled = true,
                           .session = session,
                           .fov_degrees = state.fov * 57.29578f,
                           .eye = state.eye,
                           .quaternion = quaternion,
                           .up = up,
                           .right = right,
                           .forward = forward,
                           .pitch = state.pitch,
                           .yaw = state.yaw,
                           .roll = state.roll});
}

} // Anonymous namespace

void RequestToggle() {
    toggle_requested = true;
}

bool Active() {
    return active;
}

void RequestRatioCycle() {
    ratio_cycle_requested = true;
}

std::array<u32, 2> PictureRatio() {
    if (!active) {
        return {0, 0};
    }
    return PictureRatios[ratio_index.load() % PictureRatios.size()];
}

void RequestHotsampleToggle() {
    hotsample_requested = true;
}

bool HotsampleActive() {
    return active && hotsample_active;
}

bool TakeCaptureRequest() {
    return capture_requested.exchange(false);
}

namespace Igcs {
int StartSession(u8 type) {
    (void)type; // panorama and multishot both move this camera
    if (!active) {
        return 1;
    }
    std::scoped_lock lock{igcs_mutex};
    if (igcs.active) {
        return 3;
    }
    igcs = {};
    igcs.active = true;
    igcs.start_pending = true;
    return 0;
}

void MovePanorama(f32 step_angle) {
    std::scoped_lock lock{igcs_mutex};
    if (igcs.active) {
        igcs.pan += step_angle;
    }
}

void MoveMultishot(f32 step_left_right, f32 step_up_down, f32 fov_degrees, bool from_start) {
    std::scoped_lock lock{igcs_mutex};
    if (!igcs.active) {
        return;
    }
    if (from_start) {
        igcs.left_right = step_left_right;
        igcs.up_down = step_up_down;
    } else {
        igcs.left_right += step_left_right;
        igcs.up_down += step_up_down;
    }
    if (fov_degrees > 0.0f) {
        igcs.fov_degrees = fov_degrees;
    }
}

void EndSession() {
    std::scoped_lock lock{igcs_mutex};
    if (igcs.active) {
        igcs.active = false;
        igcs.end_pending = true;
    }
}
} // namespace Igcs

void Reset() {
    toggle_requested = false;
    if (state.controller != nullptr) {
        state.controller->DisableConfiguration();
    }
    VideoCore::PhotoMode::SetHideHud(false);
    VideoCore::PhotoMode::SetHideHaze(false);
    state = {};
    ratio_index = 0;
    hotsample_active = false;
    active = false;
}

void Update(Core::System& system) {
    const u64 base = system.GetApplicationMainBase();
    if (base != state.base) {
        // Another process: nothing of the old one's photo mode applies.
        Reset();
        state.base = base;
    }
    bool toggle = toggle_requested.exchange(false);
    if (base == 0 || system.GetApplicationProcessProgramID() != ProgramId) {
        return;
    }
    auto* const controller = active ? state.controller : FirstController(system);
    Core::HID::ButtonValues buttons{};
    if (controller != nullptr) {
        buttons = controller->GetButtonsValues();
    }
    const auto down = [&](Button button) {
        return static_cast<size_t>(button) < buttons.size() && buttons[button].value;
    };
    const auto pressed = [&](Button button) { return down(button) && !state.held[button]; };
    // L3 + R3 on the controller: both down, one of them just now.
    if (down(Button::LStick) && down(Button::RStick) &&
        (pressed(Button::LStick) || pressed(Button::RStick))) {
        toggle = true;
    }
    auto& memory = system.ApplicationMemory();
    if (!active) {
        if (toggle) {
            if (!PatchInPlace(memory, base)) {
                LOG_INFO(Core, "Ocarina of Time photo mode needs the built-in Photo Mode patch "
                               "(this game version and the patch enabled)");
            } else if (Enter(system)) {
                return;
            }
        }
        RememberButtons(controller);
        return;
    }
    if (toggle || pressed(Button::B)) {
        Leave(system);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const f32 dt = std::clamp(std::chrono::duration<f32>(now - state.last).count(), 0.0f, 0.1f);
    state.last = now;

    if (pressed(Button::X)) {
        SetHudHidden(!state.hud_hidden);
    }
    if (pressed(Button::A)) {
        Freeze(memory, !state.frozen);
    }
    if (pressed(Button::Plus)) {
        capture_requested = true;
    }
    if (pressed(Button::Minus) || hotsample_requested.exchange(false)) {
        hotsample_active = !hotsample_active;
    }
    // L3 alone: the next picture ratio. R3 alone: the haze off or on again.
    if ((pressed(Button::LStick) && !down(Button::RStick)) ||
        ratio_cycle_requested.exchange(false)) {
        ratio_index = (ratio_index.load() + 1) % static_cast<u32>(PictureRatios.size());
    }
    if (pressed(Button::RStick) && !down(Button::LStick)) {
        state.haze_hidden = !state.haze_hidden;
        VideoCore::PhotoMode::SetHideHaze(state.haze_hidden);
    }
    if (pressed(Button::Y)) {
        StartFrom(state.start_camera);
        state.fov = state.start_fov;
    }
    if (state.frozen) {
        // Kept frozen: the game may set its time scale again (a scene's slow motion, say).
        Freeze(memory, true);
    }

    Core::HID::SticksValues sticks{};
    if (controller != nullptr) {
        sticks = controller->GetSticksValues();
    }
    const auto axis = [](f32 value) { return std::abs(value) < StickDeadzone ? 0.0f : value; };
    const f32 lx = axis(sticks[Settings::NativeAnalog::LStick].x.value);
    const f32 ly = axis(sticks[Settings::NativeAnalog::LStick].y.value);
    const f32 rx = axis(sticks[Settings::NativeAnalog::RStick].x.value);
    const f32 ry = axis(sticks[Settings::NativeAnalog::RStick].y.value);
    const f32 fine = down(Button::L) ? 0.25f : 1.0f;
    const f32 fast = down(Button::R) ? 4.0f : 1.0f;

    const bool igcs_session = ApplyIgcs();
    if (!igcs_session) {
        const f32 look = LookSpeed * dt * (down(Button::L) ? 0.35f : 1.0f) *
                         std::clamp(state.fov / DefaultFov, 0.15f, 1.5f);
        state.yaw -= rx * look;
        state.pitch = std::clamp(state.pitch + ry * look, -MaxPitch, MaxPitch);
        // D-pad up zooms in (a narrower field of view), down zooms out; left and right roll.
        const f32 zoom = (down(Button::DDown) ? 1.0f : 0.0f) - (down(Button::DUp) ? 1.0f : 0.0f);
        state.fov = std::clamp(state.fov + zoom * FovSpeed * dt * fine, MinFov, MaxFov);
        state.roll += ((down(Button::DRight) ? 1.0f : 0.0f) - (down(Button::DLeft) ? 1.0f : 0.0f)) *
                      RollSpeed * dt * fine;
        const f32 step = MoveSpeed * fine * fast * dt;
        const Vec3 forward = Forward(state.yaw, state.pitch);
        const Vec3 right = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
        state.eye = Add(state.eye, forward, ly * step);
        state.eye = Add(state.eye, right, lx * step);
        const f32 rise = (down(Button::ZR) ? 1.0f : 0.0f) - (down(Button::ZL) ? 1.0f : 0.0f);
        state.eye[1] += rise * step;
    }

    WriteCamera(memory);
    memory.Write32(base + StateOffset + EnabledOffset, 1);
    PublishIgcsCamera(igcs_session);
    for (size_t i = 0; i < state.held.size() && i < buttons.size(); ++i) {
        state.held[i] = buttons[i].value;
    }
}

} // namespace Core::OcarinaPhotoMode
