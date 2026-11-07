// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <vector>
#include <vulkan/vulkan.h>

namespace Vulkan {

// The callback consumes each create-info synchronously. All parts use the same
// complete pipeline layout, so independent descriptor-set layouts are unnecessary.
template <typename Create>
void ForEachGraphicsPipelineLibrary(const VkGraphicsPipelineCreateInfo& original, Create&& create) {
    bool mesh = false;
    std::vector<VkPipelineShaderStageCreateInfo> pre_raster, fragment;
    for (uint32_t index = 0; index < original.stageCount; ++index) {
        const auto& stage = original.pStages[index];
        mesh |= stage.stage == VK_SHADER_STAGE_MESH_BIT_EXT;
        (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT ? fragment : pre_raster).push_back(stage);
    }
    constexpr std::array parts{
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    };
    for (const auto part : parts) {
        if (mesh && part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT)
            continue;
        const VkGraphicsPipelineLibraryCreateInfoEXT library{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, original.pNext,
            static_cast<VkGraphicsPipelineLibraryFlagsEXT>(part)};
        auto info = original;
        info.pNext = &library;
        info.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        info.stageCount = 0;
        info.pStages = nullptr;
        info.pVertexInputState = nullptr;
        info.pInputAssemblyState = nullptr;
        info.pTessellationState = nullptr;
        info.pViewportState = nullptr;
        info.pRasterizationState = nullptr;
        info.pMultisampleState = nullptr;
        info.pDepthStencilState = nullptr;
        info.pColorBlendState = nullptr;
        if (part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) {
            info.pVertexInputState = original.pVertexInputState;
            info.pInputAssemblyState = original.pInputAssemblyState;
            info.renderPass = VK_NULL_HANDLE;
            info.layout = VK_NULL_HANDLE;
        } else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) {
            info.stageCount = static_cast<uint32_t>(pre_raster.size());
            info.pStages = pre_raster.data();
            info.pTessellationState = original.pTessellationState;
            info.pViewportState = original.pViewportState;
            info.pRasterizationState = original.pRasterizationState;
        } else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) {
            info.stageCount = static_cast<uint32_t>(fragment.size());
            info.pStages = fragment.data();
            info.pMultisampleState = original.pMultisampleState;
            info.pDepthStencilState = original.pDepthStencilState;
        } else {
            info.pMultisampleState = original.pMultisampleState;
            info.pColorBlendState = original.pColorBlendState;
            info.layout = VK_NULL_HANDLE;
        }
        create(info);
    }
}

inline VkGraphicsPipelineCreateInfo LinkedGraphicsPipelineInfo(
    const VkGraphicsPipelineCreateInfo& original, const VkPipelineLibraryCreateInfoKHR& libraries) {
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &libraries;
    info.flags = original.flags;
    info.layout = original.layout;
    info.renderPass = original.renderPass;
    info.subpass = original.subpass;
    info.basePipelineIndex = -1;
    return info;
}

} // namespace Vulkan
