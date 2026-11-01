// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>

namespace Vulkan {

constexpr bool IsTensorMatrixShape(const VkCooperativeMatrixPropertiesNV& matrix) {
    return matrix.MSize == 16 && matrix.NSize == 8 && matrix.KSize == 16 &&
           matrix.AType == VK_COMPONENT_TYPE_FLOAT16_NV &&
           matrix.BType == VK_COMPONENT_TYPE_FLOAT16_NV &&
           matrix.CType == VK_COMPONENT_TYPE_FLOAT16_NV &&
           matrix.DType == VK_COMPONENT_TYPE_FLOAT16_NV && matrix.scope == VK_SCOPE_SUBGROUP_NV;
}

} // namespace Vulkan
