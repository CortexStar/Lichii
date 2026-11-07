// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.h>
#include "video_core/renderer_vulkan/graphics_pipeline_library.h"

namespace {
using Json = nlohmann::json;

void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}

VkShaderStageFlagBits Stage(const std::string& name) {
    if (name == "vertex") return VK_SHADER_STAGE_VERTEX_BIT;
    if (name == "fragment") return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (name == "task") return VK_SHADER_STAGE_TASK_BIT_EXT;
    if (name == "mesh") return VK_SHADER_STAGE_MESH_BIT_EXT;
    throw std::runtime_error("Unsupported probe shader stage");
}

VkDescriptorType Descriptor(const std::string& name) {
    if (name == "uniform_buffer") return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (name == "storage_buffer") return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (name == "uniform_texel_buffer") return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    if (name == "storage_texel_buffer") return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    if (name == "combined_image_sampler") return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (name == "storage_image") return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    throw std::runtime_error("Unsupported probe descriptor type");
}

struct Device {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkPipelineCache cache{};
    VkRenderPass render_pass{};
    VkRenderPass hdr_render_pass{};
    VkDebugUtilsMessengerEXT messenger{};
    std::atomic<size_t> validation_errors{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
    bool mesh{};
    bool application_cache{true};
    bool libraries{};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT library_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    bool subgroup_control{};
    VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroup_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};

    static VKAPI_ATTR VkBool32 VKAPI_CALL Validate(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            auto& self = *static_cast<Device*>(user);
            if (self.validation_errors.fetch_add(1) < 8) std::cerr << data->pMessage << '\n';
            return VK_TRUE;
        }
        return VK_FALSE;
    }

    explicit Device(const std::vector<char>& initial_cache = {}, bool use_libraries = false) {
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
            "Lichii pipeline prewarm test", 1, "", 1, VK_API_VERSION_1_2};
        uint32_t layer_count{};
        Check(vkEnumerateInstanceLayerProperties(&layer_count, nullptr));
        std::vector<VkLayerProperties> layers(layer_count);
        Check(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()));
        bool validation = false;
        for (const auto& layer : layers) validation |= std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
        if (!validation) throw std::runtime_error("VK_LAYER_KHRONOS_validation is required for this test");
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        const VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            nullptr, 0, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            &Validate, this};
        const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &debug_info,
            0, &app, 1, &layer, 1, &debug_extension};
        Check(vkCreateInstance(&instance_info, nullptr, &instance));
        const auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!create_debug) throw std::runtime_error("Debug messenger unavailable");
        Check(create_debug(instance, &debug_info, nullptr, &messenger));
        uint32_t count{};
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        uint32_t family = ~0U;
        for (const auto candidate : devices) {
            uint32_t families{};
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (uint32_t index = 0; index < families; ++index) {
                if (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    physical = candidate;
                    family = index;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("No graphics device");
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2) throw std::runtime_error("Vulkan 1.2 required");
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()));
        for (const auto& extension : extensions) {
            mesh |= std::string_view(extension.extensionName) == VK_EXT_MESH_SHADER_EXTENSION_NAME;
            subgroup_control |= std::string_view(extension.extensionName) == VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
        }
        VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroup_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        features11.pNext = &features12;
        features12.pNext = subgroup_control ? static_cast<void*>(&subgroup_features)
                                           : mesh ? static_cast<void*>(&mesh_features) : nullptr;
        subgroup_features.pNext = mesh ? &mesh_features : nullptr;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features11};
        vkGetPhysicalDeviceFeatures2(physical, &features);
        mesh &= mesh_features.meshShader && mesh_features.taskShader;
        mesh_features.multiviewMeshShader = VK_FALSE;
        mesh_features.primitiveFragmentShadingRateMeshShader = VK_FALSE;
        mesh_features.meshShaderQueries = VK_FALSE;
        subgroup_control &= subgroup_features.subgroupSizeControl != VK_FALSE;
        features12.pNext = subgroup_control ? static_cast<void*>(&subgroup_features)
                                           : mesh ? static_cast<void*>(&mesh_features) : nullptr;
        subgroup_features.pNext = mesh ? &mesh_features : nullptr;
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
        bool library_extension = false;
        for (const auto& extension : extensions) {
            library_extension |= std::string_view(extension.extensionName) == VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        }
        if (use_libraries) {
            if (!library_extension) throw std::runtime_error("Graphics pipeline libraries unavailable");
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &library_features};
            vkGetPhysicalDeviceFeatures2(physical, &query);
            if (!library_features.graphicsPipelineLibrary) throw std::runtime_error("Graphics pipeline library feature unavailable");
            libraries = true;
            if (mesh) mesh_features.pNext = &library_features;
            else if (subgroup_control) subgroup_features.pNext = &library_features;
            else features12.pNext = &library_features;
            VkPhysicalDeviceProperties2 query_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &library_properties};
            vkGetPhysicalDeviceProperties2(physical, &query_properties);
        }
        const float priority = 1;
        const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr,
            0, family, 1, &priority};
        std::vector<const char*> enabled_extensions;
        if (libraries) {
            enabled_extensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
            enabled_extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        }
        if (mesh) enabled_extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        if (subgroup_control) enabled_extensions.push_back(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
        const VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features11, 0,
            1, &queue, 0, nullptr, static_cast<uint32_t>(enabled_extensions.size()), enabled_extensions.data(), &features.features};
        Check(vkCreateDevice(physical, &info, nullptr, &device));
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = subgroup_control ? static_cast<void*>(&subgroup_properties)
                                            : mesh ? static_cast<void*>(&mesh_properties) : nullptr;
        subgroup_properties.pNext = mesh ? &mesh_properties : nullptr;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        if (properties.limits.maxColorAttachments < 8) throw std::runtime_error("Eight color attachments are required");
        if (!initial_cache.empty()) {
            VkPipelineCacheHeaderVersionOne header{};
            if (initial_cache.size() < sizeof(header)) throw std::runtime_error("Truncated driver cache");
            std::memcpy(&header, initial_cache.data(), sizeof(header));
            if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                header.vendorID != properties.vendorID || header.deviceID != properties.deviceID ||
                std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE)) {
                throw std::runtime_error("Driver cache belongs to another device or driver");
            }
        }
        const VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            nullptr, 0, initial_cache.size(), initial_cache.data()};
        Check(vkCreatePipelineCache(device, &cache_info, nullptr, &cache));
        std::array<VkAttachmentDescription, 9> attachments{};
        std::array<VkAttachmentReference, 8> colors{};
        for (uint32_t index = 0; index < colors.size(); ++index) {
            attachments[index].format = VK_FORMAT_R8G8B8A8_UNORM;
            attachments[index].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[index].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments[index].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[index] = {index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        attachments[8].format = VK_FORMAT_D32_SFLOAT;
        attachments[8].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[8].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[8].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference depth{8, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colors.size());
        subpass.pColorAttachments = colors.data();
        subpass.pDepthStencilAttachment = &depth;
        const VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            nullptr, 0, static_cast<uint32_t>(attachments.size()), attachments.data(), 1, &subpass};
        Check(vkCreateRenderPass(device, &pass_info, nullptr, &render_pass));
        VkFormatProperties hdr_properties{};
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &hdr_properties);
        if (hdr_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
            attachments[0].format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            Check(vkCreateRenderPass(device, &pass_info, nullptr, &hdr_render_pass));
        }
    }

    ~Device() {
        if (hdr_render_pass) vkDestroyRenderPass(device, hdr_render_pass, nullptr);
        if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
        if (cache) vkDestroyPipelineCache(device, cache, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (messenger) {
            const auto destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_debug) destroy_debug(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

Json Build(Device& device, const Json& group, const std::filesystem::path& directory,
           std::ostream& trace) {
    std::vector<VkShaderModule> modules;
    std::vector<VkPipeline> library_handles;
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout layout{};
    const auto cleanup = [&] {
        for (const auto handle : library_handles) vkDestroyPipeline(device.device, handle, nullptr);
        library_handles.clear();
        if (layout) vkDestroyPipelineLayout(device.device, layout, nullptr);
        if (descriptor_layout) vkDestroyDescriptorSetLayout(device.device, descriptor_layout, nullptr);
        for (const auto module : modules) vkDestroyShaderModule(device.device, module, nullptr);
        layout = {};
        descriptor_layout = {};
        modules.clear();
    };
    try {
        const size_t initial_errors = device.validation_errors.load();
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
            nullptr, 32};
        std::map<uint32_t, VkDescriptorSetLayoutBinding> bindings;
        bool is_mesh = false;
        const auto& layouts = group.at("descriptor_layouts");
        if (layouts.size() != group.at("modules").size()) throw std::runtime_error("Module/layout count mismatch");
        for (size_t index = 0; index < layouts.size(); ++index) {
            const auto stage = Stage(layouts[index].at("stage"));
            is_mesh |= stage == VK_SHADER_STAGE_MESH_BIT_EXT;
            if ((stage == VK_SHADER_STAGE_MESH_BIT_EXT || stage == VK_SHADER_STAGE_TASK_BIT_EXT) && !device.mesh) {
                throw std::runtime_error("Mesh shader feature unavailable");
            }
            std::ifstream file(directory / group.at("modules")[index].get<std::string>(), std::ios::binary | std::ios::ate);
            const auto size = file.tellg();
            if (size < 20 || size > 64 * 1024 * 1024 || size % 4) throw std::runtime_error("Invalid SPIR-V size");
            std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(words.data()), size)) throw std::runtime_error("SPIR-V read failed");
            const VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                nullptr, 0, static_cast<size_t>(size), words.data()};
            VkShaderModule module{};
            Check(vkCreateShaderModule(device.device, &shader, nullptr, &module));
            modules.push_back(module);
            const bool warp_ops = layouts[index].value("uses_warp_ops", false);
            const bool force_subgroup = warp_ops && device.subgroup_control &&
                (device.subgroup_properties.requiredSubgroupSizeStages & stage) &&
                device.subgroup_properties.minSubgroupSize <= 32 && device.subgroup_properties.maxSubgroupSize >= 32;
            stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                              stage, module, "main", nullptr});
            if (force_subgroup) stages.back().pNext = &subgroup;
            for (const auto& row : layouts[index].at("bindings")) {
                const uint32_t binding = row.at("binding"), count = row.at("count");
                if (binding >= 256 || count != 1) throw std::runtime_error("Unsupported descriptor layout");
                const auto type = Descriptor(row.at("kind"));
                const auto [it, inserted] = bindings.emplace(binding,
                    VkDescriptorSetLayoutBinding{binding, type, count, stage, nullptr});
                if (!inserted) {
                    if (it->second.descriptorType != type) throw std::runtime_error("Conflicting descriptor types");
                    it->second.stageFlags |= stage;
                }
            }
        }
        if (is_mesh && (group.at("mesh_contract").at("max_vertices").get<uint32_t>() > device.mesh_properties.maxMeshOutputVertices ||
                        group.at("mesh_contract").at("max_primitives").get<uint32_t>() > device.mesh_properties.maxMeshOutputPrimitives)) {
            throw std::runtime_error("Mesh output exceeds device limits");
        }
        std::vector<VkDescriptorSetLayoutBinding> rows;
        for (const auto& [index, row] : bindings) rows.push_back(row);
        const VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr, 0, static_cast<uint32_t>(rows.size()), rows.data()};
        Check(vkCreateDescriptorSetLayout(device.device, &descriptors, nullptr, &descriptor_layout));
        VkShaderStageFlags flags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        if (device.mesh) flags |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
        const VkPushConstantRange push{flags, 0, 44};
        const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            nullptr, 0, 1, &descriptor_layout, 1, &push};
        Check(vkCreatePipelineLayout(device.device, &layout_info, nullptr, &layout));
        std::array<VkVertexInputAttributeDescription, 32> attributes{};
        for (uint32_t index = 0; index < attributes.size(); ++index) {
            attributes[index] = {index, 0, VK_FORMAT_R32G32B32A32_SFLOAT, index * 16};
        }
        const VkVertexInputBindingDescription vertex_binding{0, 512, VK_VERTEX_INPUT_RATE_VERTEX};
        const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            nullptr, 0, 1, &vertex_binding, static_cast<uint32_t>(attributes.size()), attributes.data()};
        const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        const VkViewport viewport{0, 0, 640, 480, 0, 1};
        const VkRect2D scissor{{0, 0}, {640, 480}};
        const VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            nullptr, 0, 1, &viewport, 1, &scissor};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        std::array<VkPipelineColorBlendAttachmentState, 8> colors{};
        for (auto& color : colors) {
            color.colorWriteMask = 15;
            color.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        }
        const VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, static_cast<uint32_t>(colors.size()), colors.data()};
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline.stageCount = static_cast<uint32_t>(stages.size());
        pipeline.pStages = stages.data();
        pipeline.pVertexInputState = is_mesh ? nullptr : &vertex;
        pipeline.pInputAssemblyState = is_mesh ? nullptr : &assembly;
        pipeline.pViewportState = &view;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &samples;
        pipeline.pDepthStencilState = &depth;
        pipeline.pColorBlendState = &blend;
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        const VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
            static_cast<uint32_t>(dynamic_states.size()), dynamic_states.data()};
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = layout;
        pipeline.renderPass = device.render_pass;
        Json library_build_ms = Json::array();
        std::vector<VkPipeline> linked;
        const auto build_libraries = [&] {
            linked.clear();
            Vulkan::ForEachGraphicsPipelineLibrary(pipeline, [&](const VkGraphicsPipelineCreateInfo& info) {
                VkPipeline handle{};
                const auto start = std::chrono::steady_clock::now();
                const auto result = vkCreateGraphicsPipelines(device.device,
                    device.application_cache ? device.cache : VK_NULL_HANDLE, 1, &info, nullptr, &handle);
                if (handle) library_handles.push_back(handle);
                Check(result);
                library_build_ms.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count());
                if (device.validation_errors.load() != initial_errors) throw std::runtime_error("Vulkan library validation failed");
                linked.push_back(handle);
            });
        };
        Json total_times = Json::array();
        Json times = Json::array();
        for (uint32_t trial = 0; trial < (device.hdr_render_pass ? 6U : 4U); ++trial) {
            const auto trial_start = std::chrono::steady_clock::now();
            if (trial == 2) {
                for (auto& color : colors) color.blendEnable = VK_TRUE;
                depth.depthTestEnable = VK_TRUE;
                depth.depthWriteEnable = VK_TRUE;
            }
            if (trial == 4) pipeline.renderPass = device.hdr_render_pass;
            if (device.libraries) build_libraries();
            VkPipeline handle{};
            trace << "trial " << trial << " begin\n" << std::flush;
            const auto start = std::chrono::steady_clock::now();
            VkPipelineLibraryCreateInfoKHR link{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                nullptr, static_cast<uint32_t>(linked.size()), linked.data()};
            const auto executable = Vulkan::LinkedGraphicsPipelineInfo(pipeline, link);
            const VkResult result = vkCreateGraphicsPipelines(device.device,
                device.application_cache ? device.cache : VK_NULL_HANDLE, 1, device.libraries ? &executable : &pipeline, nullptr, &handle);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (handle) vkDestroyPipeline(device.device, handle, nullptr);
            trace << "trial " << trial << " end result=" << result << " ms=" << ms << '\n' << std::flush;
            Check(result);
            if (device.validation_errors.load() != initial_errors) throw std::runtime_error("Vulkan validation failed");
            times.push_back(ms);
            total_times.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - trial_start).count());
        }
        cleanup();
        return {{"status", "compiled"}, {"first_ms", times[0]}, {"repeat_ms", times[1]},
                {"changed_state_ms", times[2]}, {"changed_repeat_ms", times[3]},
                {"library_build_ms", library_build_ms}, {"trial_total_ms", total_times},
                {"initial_with_libraries_ms", total_times[0]},
                {"changed_with_libraries_ms", total_times[2]},
                {"hdr_with_libraries_ms", total_times.size() > 4 ? total_times[4] : Json{}},
                {"hdr_repeat_with_libraries_ms", total_times.size() > 5 ? total_times[5] : Json{}}};
    } catch (...) {
        cleanup();
        throw;
    }
}
} // namespace

int main(int argc, char** argv) try {
    if (argc < 3 || argc > 6) return 2;
    const size_t limit = argc >= 4 ? std::stoull(argv[3]) : (std::numeric_limits<size_t>::max)();
    const bool libraries = (argc == 5 && std::string_view(argv[4]) == "--libraries") ||
                           (argc == 6 && std::string_view(argv[5]) == "--libraries");
    if (argc == 6 && !libraries) return 2;
    const bool no_cache = argc >= 5 && std::string_view(argv[4]) == "--no-app-cache";
    std::vector<char> initial_cache;
    if (argc >= 5 && !no_cache && std::string_view(argv[4]) != "--libraries") {
        std::ifstream cache_file(argv[4], std::ios::binary | std::ios::ate);
        const auto size = cache_file.tellg();
        if (size < 0 || size > 512 * 1024 * 1024) throw std::runtime_error("Invalid input cache size");
        initial_cache.resize(static_cast<size_t>(size));
        cache_file.seekg(0);
        if (!cache_file.read(initial_cache.data(), size)) throw std::runtime_error("Cannot read input cache");
    }
    const std::filesystem::path input{argv[1]};
    std::ifstream source(input);
    Json report;
    source >> report;
    Device device(initial_cache, libraries);
    device.application_cache = !no_cache;
    Json results = Json::array();
    std::ofstream trace(std::string(argv[2]) + ".trace");
    if (!trace) throw std::runtime_error("Cannot create progress trace");
    size_t failed = 0;
    for (const auto& group : report.at("results")) {
        if (group.at("status") != "compiled_under_contract") continue;
        if (results.size() >= limit) break;
        Json result{{"container_offset", group.at("container_offset")}, {"variation", group.at("variation")}};
        trace << "group " << group.at("container_offset") << ':' << group.at("variation") << '\n' << std::flush;
        try { result.update(Build(device, group, input.parent_path(), trace)); }
        catch (const std::exception& error) {
            result["status"] = "failed";
            result["error"] = error.what();
            ++failed;
        }
        trace << result.dump() << '\n' << std::flush;
        results.push_back(std::move(result));
    }
    Json output{{"device", device.properties.deviceName}, {"driver_version", device.properties.driverVersion},
                {"note", "Pipeline creation only; no draw or dispatch. First is not guaranteed driver-cache-cold."},
                {"application_cache", device.application_cache},
                {"graphics_pipeline_libraries", device.libraries},
                {"fast_linking", bool(device.library_properties.graphicsPipelineLibraryFastLinking)},
                {"independent_interpolation", bool(device.library_properties.graphicsPipelineLibraryIndependentInterpolationDecoration)},
                {"loaded_cache_bytes", initial_cache.size()},
                {"validation_errors", device.validation_errors.load()}, {"results", results}};
    if (device.application_cache) {
        size_t size{};
        Check(vkGetPipelineCacheData(device.device, device.cache, &size, nullptr));
        if (size > 512 * 1024 * 1024) throw std::runtime_error("Driver cache exceeds test size limit");
        std::vector<char> bytes(size);
        Check(vkGetPipelineCacheData(device.device, device.cache, &size, bytes.data()));
        const auto path = std::string(argv[2]) + ".cache.bin";
        std::ofstream cache_file(path, std::ios::binary);
        if (!cache_file.write(bytes.data(), size)) throw std::runtime_error("Cannot write driver cache");
        output["cache_bytes"] = size;
        output["cache_file"] = path;
    }
    std::ofstream destination(argv[2]);
    destination << output.dump(2) << '\n';
    if (!destination) throw std::runtime_error("Cannot save timing report");
    std::cout << device.properties.deviceName << ": " << results.size() << " pipeline groups tested\n";
    return failed ? 1 : 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
