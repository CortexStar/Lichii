// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

/// The GPU virtual address ranges of buffers, images and memory as VK_EXT_device_address_binding_
/// report announces them (EDEN_NX2_GPU_CRUMBS=1), so a device fault address names the object it
/// lies in, the one it lay in before that object was freed, or the objects next to it.
namespace Vulkan::AddressBindings {

struct Binding {
    u64 base;
    u64 size;
    VkObjectType type;
    u64 object;
    std::string what; ///< The create call that bound it, or the object's debug name.
    u64 sequence;     ///< Order of binding (and of unbinding for freed ranges).
};

class History {
public:
    explicit History(size_t freed_capacity = 4096) : capacity{freed_capacity} {}

    void Bind(u64 base, u64 size, VkObjectType type, u64 object, std::string what);
    void Unbind(u64 base, u64 size, VkObjectType type, u64 object);

    /// What `address` (reported with `precision`, a power of two or 0) lies in, one line each:
    /// the live ranges containing it, else the most recently freed ones, else its neighbours.
    [[nodiscard]] std::vector<std::string> Explain(u64 address, u64 precision) const;

    [[nodiscard]] size_t LiveCount() const;

private:
    mutable std::mutex mutex;
    size_t capacity;
    u64 sequence{};
    std::multimap<u64, Binding> live; ///< By base address.
    std::deque<Binding> freed;        ///< Oldest first.
};

/// The process-wide history the debug messenger feeds; null unless EDEN_NX2_GPU_CRUMBS is on.
[[nodiscard]] History* Global();

/// Feeds one VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT message to Global().
void OnMessage(const VkDebugUtilsMessengerCallbackDataEXT& data);

} // namespace Vulkan::AddressBindings
