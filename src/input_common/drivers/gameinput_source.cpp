// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>
#include <vector>

#include <windows.h>

#include <GameInput.h>

#include "common/logging.h"
#include "input_common/drivers/gameinput_source.h"

namespace InputCommon {

namespace Api = GameInput::v3;

/// GameInputInitialize, exported by GameInputRedist.dll and recent GameInput.dll. Looked up at
/// run time: Lichii links no GameInput library.
using InitializeFn = HRESULT(WINAPI*)(REFIID riid, LPVOID* object);

struct GameInputSource::Impl {
    struct PadState {
        Api::IGameInputDevice* device;
        GameInputMapping::GamepadState state;
    };

    ~Impl() {
        for (const Api::GameInputCallbackToken token : {system_token, reading_token, device_token}) {
            if (token != 0) {
                input->StopCallback(token);
                input->UnregisterCallback(token);
            }
        }
        for (const PadState& pad : pads) {
            pad.device->Release();
        }
        if (input != nullptr) {
            input->Release();
        }
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }

    static void CALLBACK OnDevice(Api::GameInputCallbackToken, void* context,
                                  Api::IGameInputDevice* device, uint64_t,
                                  Api::GameInputDeviceStatus current,
                                  Api::GameInputDeviceStatus previous) {
        auto& impl = *static_cast<Impl*>(context);
        const bool now = (current & Api::GameInputDeviceConnected) != 0;
        const bool before = (previous & Api::GameInputDeviceConnected) != 0;
        if (now && !before) {
            const Api::GameInputDeviceInfo* info = nullptr;
            if (FAILED(device->GetDeviceInfo(&info)) || info == nullptr) {
                return;
            }
            const Device description{
                .vendor_id = info->vendorId,
                .product_id = info->productId,
                .family = static_cast<int>(info->deviceFamily),
                .supported_input = static_cast<u32>(info->supportedInput),
                .name = info->displayName != nullptr ? info->displayName : "GameInput gamepad",
            };
            device->AddRef();
            {
                std::scoped_lock lock{impl.mutex};
                impl.pads.push_back({device, {}});
            }
            LOG_INFO(Input, "GameInput gamepad connected: {} ({:04x}:{:04x}, family {}, input {:#x})",
                     description.name, description.vendor_id, description.product_id,
                     description.family, description.supported_input);
            impl.callbacks.connected(device, description);
        } else if (!now && before) {
            {
                std::scoped_lock lock{impl.mutex};
                const auto it = std::ranges::find(impl.pads, device, &PadState::device);
                if (it == impl.pads.end()) {
                    return;
                }
                impl.pads.erase(it);
            }
            impl.callbacks.disconnected(device);
            device->Release();
        }
    }

    static void CALLBACK OnReading(Api::GameInputCallbackToken, void* context,
                                   Api::IGameInputReading* reading) {
        auto& impl = *static_cast<Impl*>(context);
        Api::GameInputGamepadState raw{};
        if (!reading->GetGamepadState(&raw)) {
            return;
        }
        Api::IGameInputDevice* device = nullptr;
        reading->GetDevice(&device);
        if (device == nullptr) {
            return;
        }
        GameInputMapping::GamepadState state;
        bool known = false;
        {
            std::scoped_lock lock{impl.mutex};
            const auto it = std::ranges::find(impl.pads, device, &PadState::device);
            if (it != impl.pads.end()) {
                it->state.buttons = static_cast<u32>(raw.buttons);
                it->state.left_trigger = raw.leftTrigger;
                it->state.right_trigger = raw.rightTrigger;
                it->state.left_x = raw.leftThumbstickX;
                it->state.left_y = raw.leftThumbstickY;
                it->state.right_x = raw.rightThumbstickX;
                it->state.right_y = raw.rightThumbstickY;
                state = it->state;
                known = true;
            }
        }
        if (known) {
            impl.callbacks.state(device, state);
        }
        device->Release();
    }

    static void CALLBACK OnSystemButtons(Api::GameInputCallbackToken, void* context,
                                         Api::IGameInputDevice* device, uint64_t,
                                         Api::GameInputSystemButtons current,
                                         Api::GameInputSystemButtons) {
        auto& impl = *static_cast<Impl*>(context);
        GameInputMapping::GamepadState state;
        {
            std::scoped_lock lock{impl.mutex};
            const auto it = std::ranges::find(impl.pads, device, &PadState::device);
            if (it == impl.pads.end()) {
                return;
            }
            it->state.system_buttons = static_cast<u32>(current);
            state = it->state;
        }
        impl.callbacks.state(device, state);
    }

    HMODULE module{};
    Api::IGameInput* input{};
    Callbacks callbacks;
    std::mutex mutex;
    std::vector<PadState> pads;
    Api::GameInputCallbackToken device_token{};
    Api::GameInputCallbackToken reading_token{};
    Api::GameInputCallbackToken system_token{};
};

namespace {

/// GameInputInitialize from the redistributable in System32, else from an inbox GameInput.dll
/// new enough to export it (older inbox ones only have the API version 0 GameInputCreate).
InitializeFn LoadInitialize(HMODULE& module) {
    for (const wchar_t* name : {L"GameInputRedist.dll", L"GameInput.dll"}) {
        module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            continue;
        }
        if (const FARPROC proc = GetProcAddress(module, "GameInputInitialize")) {
            return reinterpret_cast<InitializeFn>(reinterpret_cast<void*>(proc));
        }
        FreeLibrary(module);
        module = nullptr;
    }
    return nullptr;
}

} // Anonymous namespace

std::unique_ptr<GameInputSource> GameInputSource::Create(Callbacks callbacks) {
    auto impl = std::make_unique<Impl>();
    impl->callbacks = std::move(callbacks);
    const InitializeFn initialize = LoadInitialize(impl->module);
    if (initialize == nullptr) {
        LOG_INFO(Input, "GameInput redistributable not installed, SDL reads all controllers");
        return nullptr;
    }
    HRESULT result =
        initialize(Api::IID_IGameInput, reinterpret_cast<LPVOID*>(&impl->input));
    if (FAILED(result) || impl->input == nullptr) {
        impl->input = nullptr;
        LOG_WARNING(Input, "GameInput API version 3 unavailable ({:#x}), SDL reads all controllers",
                    static_cast<u32>(result));
        return nullptr;
    }
    Impl* const context = impl.get();
    result = impl->input->RegisterDeviceCallback(
        nullptr, Api::GameInputKindGamepad, Api::GameInputDeviceConnected,
        Api::GameInputBlockingEnumeration, context, &Impl::OnDevice, &impl->device_token);
    if (SUCCEEDED(result)) {
        result = impl->input->RegisterReadingCallback(nullptr, Api::GameInputKindGamepad, context,
                                                      &Impl::OnReading, &impl->reading_token);
    }
    if (SUCCEEDED(result)) {
        result = impl->input->RegisterSystemButtonCallback(
            nullptr, Api::GameInputSystemButtonGuide | Api::GameInputSystemButtonShare, context,
            &Impl::OnSystemButtons, &impl->system_token);
    }
    if (FAILED(result)) {
        LOG_WARNING(Input, "GameInput callbacks unavailable ({:#x}), SDL reads all controllers",
                    static_cast<u32>(result));
        return nullptr;
    }
    LOG_INFO(Input, "GameInput reads gamepads (API version 3, reading callbacks)");
    return std::unique_ptr<GameInputSource>{new GameInputSource{std::move(impl)}};
}

GameInputSource::GameInputSource(std::unique_ptr<Impl> impl_) : impl{std::move(impl_)} {}

GameInputSource::~GameInputSource() = default;

void GameInputSource::Rumble(Pad pad, float low_frequency, float high_frequency) {
    std::scoped_lock lock{impl->mutex};
    const auto it = std::ranges::find(impl->pads, pad, [](const Impl::PadState& state) {
        return static_cast<Pad>(state.device);
    });
    if (it == impl->pads.end()) {
        return;
    }
    const Api::GameInputRumbleParams params{
        .lowFrequency = std::clamp(low_frequency, 0.0f, 1.0f),
        .highFrequency = std::clamp(high_frequency, 0.0f, 1.0f),
        .leftTrigger = 0.0f,
        .rightTrigger = 0.0f,
    };
    it->device->SetRumbleState(&params);
}

} // namespace InputCommon
