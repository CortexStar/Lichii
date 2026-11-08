// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>

#include <fmt/format.h>

#include "video_core/vulkan_common/address_bindings.h"
#include "video_core/vulkan_common/driver_call.h"
#include "video_core/vulkan_common/gpu_crumbs.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"

namespace Vulkan::AddressBindings {
namespace {

std::string_view TypeName(VkObjectType type) {
    std::string_view name = string_VkObjectType(type);
    if (name.starts_with("VK_OBJECT_TYPE_")) {
        name.remove_prefix(15);
    }
    return name;
}

std::string Name(const Binding& binding) {
    return fmt::format("{} {:#x} [{:#x}, {:#x})", TypeName(binding.type), binding.object,
                       binding.base, binding.base + binding.size);
}

constexpr size_t MaxLines = 4;
/// Faults below this are a null address plus an offset; no object is ever bound there.
constexpr u64 NullPageEnd = 0x10000;

} // Anonymous namespace

void History::Bind(u64 base, u64 size, VkObjectType type, u64 object, std::string what) {
    std::scoped_lock lock{mutex};
    live.emplace(base, Binding{base, size, type, object, std::move(what), ++sequence});
}

void History::Unbind(u64 base, u64 size, VkObjectType type, u64 object) {
    std::scoped_lock lock{mutex};
    auto [begin, end] = live.equal_range(base);
    for (auto it = begin; it != end; ++it) {
        if (it->second.object == object && it->second.type == type) {
            Binding binding = std::move(it->second);
            live.erase(it);
            binding.size = size;
            binding.sequence = ++sequence;
            freed.push_back(std::move(binding));
            if (freed.size() > capacity) {
                freed.pop_front();
            }
            return;
        }
    }
}

size_t History::LiveCount() const {
    std::scoped_lock lock{mutex};
    return live.size();
}

std::vector<std::string> History::Explain(u64 address, u64 precision) const {
    std::scoped_lock lock{mutex};
    const u64 lo = precision > 1 ? address & ~(precision - 1) : address;
    const u64 hi = lo + (std::max)(precision, u64{1});
    std::vector<std::string> lines;
    // Live ranges overlapping [lo, hi).
    for (auto it = live.begin(); it != live.end() && it->first < hi; ++it) {
        const Binding& binding = it->second;
        if (binding.base + binding.size > lo && lines.size() < MaxLines) {
            lines.push_back(fmt::format("in {} at +{:#x}: {}", Name(binding),
                                        (std::max)(lo, binding.base) - binding.base, binding.what));
        }
    }
    if (!lines.empty()) {
        return lines;
    }
    if (hi <= NullPageEnd) {
        lines.push_back("null address: a descriptor or device address that was never set, or "
                        "a small offset from one");
        return lines;
    }
    // Freed ranges, newest first: a use after free.
    bool found_freed = false;
    for (auto it = freed.rbegin(); it != freed.rend() && lines.size() < MaxLines; ++it) {
        if (it->base < hi && it->base + it->size > lo) {
            lines.push_back(fmt::format(
                "in freed {} at +{:#x} ({} binding changes since it was freed): {}", Name(*it),
                (std::max)(lo, it->base) - it->base, sequence - it->sequence, it->what));
            found_freed = true;
        }
    }
    if (found_freed) {
        return lines;
    }
    // Nothing there: the nearest live ranges below and above (an offset past an end).
    const Binding* below{};
    for (auto it = live.begin(); it != live.end() && it->first < hi; ++it) {
        if (it->second.base + it->second.size <= lo &&
            (below == nullptr || it->second.base + it->second.size > below->base + below->size)) {
            below = &it->second;
        }
    }
    const auto above = live.lower_bound(hi);
    if (below == nullptr && above == live.end()) {
        lines.push_back(fmt::format("no object: nothing bound ({} live ranges)", live.size()));
        return lines;
    }
    lines.push_back("no object bound here");
    if (below != nullptr) {
        lines.push_back(fmt::format("{:#x} bytes past the end of {}: {}",
                                    lo - (below->base + below->size), Name(*below), below->what));
    }
    if (above != live.end()) {
        lines.push_back(fmt::format("{:#x} bytes before {}: {}", above->first - lo,
                                    Name(above->second), above->second.what));
    }
    return lines;
}

History* Global() {
    static const std::unique_ptr<History> history =
        GpuCrumbs::Requested() ? std::make_unique<History>() : nullptr;
    return history.get();
}

void OnMessage(const VkDebugUtilsMessengerCallbackDataEXT& data) {
    History* const history = Global();
    if (history == nullptr) {
        return;
    }
    const VkDeviceAddressBindingCallbackDataEXT* binding{};
    for (auto* next = static_cast<const VkBaseInStructure*>(data.pNext); next != nullptr;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            binding = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
        }
    }
    if (binding == nullptr || data.objectCount == 0) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT& object = data.pObjects[0];
    if (binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT) {
        history->Unbind(binding->baseAddress, binding->size, object.objectType,
                        object.objectHandle);
        return;
    }
    // The driver reports a bind on the thread that binds: the create call marked there names
    // what the range is for.
    std::string what = DriverCall::DescribeCurrent();
    if (object.pObjectName != nullptr) {
        what = fmt::format("\"{}\" {}", object.pObjectName, what);
    }
    if (what.empty()) {
        what = "(bound outside a marked create call)";
    }
    history->Bind(binding->baseAddress, binding->size, object.objectType, object.objectHandle,
                  std::move(what));
}

} // namespace Vulkan::AddressBindings
