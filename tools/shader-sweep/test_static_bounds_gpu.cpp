// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
constexpr size_t BufferBytes = 2048;

void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Vulkan result " + std::to_string(result));
    }
}

struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
};

struct TestDevice {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool commands{};
    VkCommandBuffer command{};
    VkDescriptorSetLayout descriptor_layout{};
    VkPipelineLayout pipeline_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet descriptor_set{};
    std::array<Buffer, 4> buffers{};
    std::vector<VkPipeline> pipelines;
    uint32_t storage_bindings{3};

    TestDevice() {
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
            "Lichii static buffer bounds test", 1, "", 1, VK_API_VERSION_1_2};
        const VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            nullptr, 0, &app};
        Check(vkCreateInstance(&instance_info, nullptr, &instance));
        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) throw std::runtime_error("No Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        uint32_t family = ~0U;
        for (const auto candidate : devices) {
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            if (!features.shaderInt64) continue;
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
            for (uint32_t i = 0; i < families; ++i) {
                if (properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    physical = candidate;
                    family = i;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("No Vulkan compute device with int64");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.limits.maxPerStageDescriptorStorageBuffers >= 64 &&
            properties.limits.maxDescriptorSetStorageBuffers >= 64 &&
            properties.limits.maxPerStageResources >= 65) {
            storage_bindings = 64;
        }
        const float priority = 1;
        const VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            nullptr, 0, family, 1, &priority};
        VkPhysicalDeviceFeatures features{};
        features.shaderInt64 = VK_TRUE;
        const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            nullptr, 0, 1, &queue_info, 0, nullptr, 0, nullptr, &features};
        Check(vkCreateDevice(physical, &device_info, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        const VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        Check(vkCreateCommandPool(device, &pool_info, nullptr, &commands));
        const VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            nullptr, commands, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        Check(vkAllocateCommandBuffers(device, &allocation, &command));
        std::vector<VkDescriptorSetLayoutBinding> bindings(storage_bindings + 1);
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i] = {i, i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                           1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        const VkDescriptorSetLayoutCreateInfo descriptor_info{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
            static_cast<uint32_t>(bindings.size()), bindings.data()};
        Check(vkCreateDescriptorSetLayout(device, &descriptor_info, nullptr, &descriptor_layout));
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            nullptr, 0, 1, &descriptor_layout, 1, &push};
        Check(vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout));
        const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage_bindings}}};
        const VkDescriptorPoolCreateInfo descriptors_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            nullptr, 0, 1, static_cast<uint32_t>(sizes.size()), sizes.data()};
        Check(vkCreateDescriptorPool(device, &descriptors_info, nullptr, &descriptors));
        const VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            nullptr, descriptors, 1, &descriptor_layout};
        Check(vkAllocateDescriptorSets(device, &set_info, &descriptor_set));
        std::cout << properties.deviceName << '\n';
        for (auto& buffer : buffers) CreateBuffer(buffer);
    }

    void CreateBuffer(Buffer& buffer) {
        const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, BufferBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_SHARING_MODE_EXCLUSIVE};
        Check(vkCreateBuffer(device, &info, nullptr, &buffer.buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        uint32_t type = ~0U;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            constexpr auto wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1U << i)) &&
                (properties.memoryTypes[i].propertyFlags & wanted) == wanted) { type = i; break; }
        }
        if (type == ~0U) throw std::runtime_error("No coherent host memory");
        const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            nullptr, requirements.size, type};
        Check(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory));
        Check(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0));
        Check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    }

    VkPipeline Pipeline(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto length = input.tellg();
        if (length <= 0 || length % 4) throw std::runtime_error("Invalid SPIR-V file");
        std::vector<uint32_t> code(static_cast<size_t>(length) / 4);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(code.data()), length)) throw std::runtime_error("SPIR-V read failed");
        const VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            nullptr, 0, code.size() * 4, code.data()};
        VkShaderModule module{};
        Check(vkCreateShaderModule(device, &module_info, nullptr, &module));
        const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        const VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            nullptr, 0, stage, pipeline_layout};
        VkPipeline pipeline{};
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Check(result);
        pipelines.push_back(pipeline);
        return pipeline;
    }

    void Run(VkPipeline pipeline, uint32_t offset, uint32_t range, uint64_t mask,
             uint32_t expected, uint64_t base = 0x100000) {
        for (auto& buffer : buffers) std::memset(buffer.mapped, 0, BufferBytes);
        auto* constants = static_cast<uint32_t*>(buffers[0].mapped);
        constants[1] = offset;
        const std::array<uint64_t, 3> addresses{base, base + 16, 0x200000000ULL};
        for (size_t i = 0; i < addresses.size(); ++i) std::memcpy(constants + 4 + i * 4, &addresses[i], 8);
        auto* a = static_cast<uint32_t*>(buffers[1].mapped);
        for (uint32_t i = 0; i < 64; ++i) a[i] = 100 + i;
        *static_cast<uint32_t*>(buffers[2].mapped) = 777;
        std::vector<VkDescriptorBufferInfo> infos(storage_bindings + 1, {buffers[2].buffer, 0, 4});
        infos[0] = {buffers[0].buffer, 0, 64};
        infos[1] = {buffers[1].buffer, 0, range};
        infos[3] = {buffers[3].buffer, 0, 16};
        UpdateDescriptors(infos);
        Dispatch(pipeline, mask);
        const uint32_t actual = *static_cast<uint32_t*>(buffers[3].mapped);
        if (actual != expected) throw std::runtime_error("offset=" + std::to_string(offset) +
            " range=" + std::to_string(range) + " mask=" + std::to_string(mask) +
            " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual));
    }

    void RunHighBinding(VkPipeline pipeline, uint32_t target, uint64_t mask,
                        uint32_t expected_output, uint32_t expected_marker) {
        for (auto& buffer : buffers) std::memset(buffer.mapped, 0, BufferBytes);
        auto* constants = static_cast<uint32_t*>(buffers[0].mapped);
        constants[0] = target;
        for (uint32_t index = 0; index < storage_bindings; ++index) {
            const uint64_t address = index == 0 ? 0x200000000ULL : 0x300000000ULL + index * 0x10000ULL;
            std::memcpy(constants + 4 + index * 4, &address, sizeof(address));
        }
        auto* input = static_cast<uint32_t*>(buffers[1].mapped);
        input[0] = 1000 + target;
        input[1] = 0xdeadbeefU;
        *static_cast<uint32_t*>(buffers[2].mapped) = 777;
        *static_cast<uint32_t*>(buffers[3].mapped) = 0xdeadbeefU;
        std::vector<VkDescriptorBufferInfo> infos(storage_bindings + 1, {buffers[2].buffer, 0, 4});
        infos[0] = {buffers[0].buffer, 0, BufferBytes};
        infos[1] = {buffers[3].buffer, 0, 16};
        infos[target + 1] = {buffers[1].buffer, 0, 16};
        UpdateDescriptors(infos);
        Dispatch(pipeline, mask);
        if (*static_cast<uint32_t*>(buffers[3].mapped) != expected_output || input[1] != expected_marker) {
            throw std::runtime_error("high binding " + std::to_string(target) +
                                     " load/store validity failed for mask " + std::to_string(mask));
        }
    }

    void UpdateDescriptors(const std::vector<VkDescriptorBufferInfo>& infos) {
        std::vector<VkWriteDescriptorSet> writes(infos.size());
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, i, 0, 1,
                         i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         nullptr, &infos[i], nullptr};
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    void Dispatch(VkPipeline pipeline, uint64_t mask) {
        Check(vkResetCommandBuffer(command, 0));
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
        vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 24, 8, &mask);
        vkCmdDispatch(command, 1, 1, 1);
        const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                     VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(command));
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command};
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(queue));
    }

    ~TestDevice() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto p : pipelines) vkDestroyPipeline(device, p, nullptr);
            for (auto& b : buffers) {
                if (b.mapped) vkUnmapMemory(device, b.memory);
                if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
                if (b.memory) vkFreeMemory(device, b.memory, nullptr);
            }
            vkDestroyDescriptorPool(device, descriptors, nullptr);
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            vkDestroyCommandPool(device, commands, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
} // namespace

int main(int argc, char** argv) try {
    if (argc != 2) return 2;
    TestDevice test;
    size_t cases = 0;
    for (uint32_t width : {4U, 8U, 16U}) {
        for (uint32_t alignment : {4U, 16U}) {
            const auto pipeline = test.Pipeline(std::filesystem::path{argv[1]} /
                (std::to_string(width) + "-" + std::to_string(alignment) + ".spv"));
            test.Run(pipeline, 16, 32, 5, 104); // nearer invalid B must not win
            test.Run(pipeline, 20, 64, 7, 105); // B's four-byte range has ended
            test.Run(pipeline, 32 - width, 32, 5, 100 + (32 - width) / 4);
            test.Run(pipeline, 36 - width, 32, 5, 0); // whole access must fit
            test.Run(pipeline, 0, 32, 4, 0); // no valid input buffers
            test.Run(pipeline, 0, 32, 5, 100, 0x100100000ULL); // preserve high address bits
            test.Run(pipeline, 0, 32, (uint64_t{1} << 32) | 4, 0);
            test.Run(pipeline, 0, 32, (uint64_t{1} << 63) | 5, 100);
            cases += 8;
            if (width == 4) { test.Run(pipeline, 16, 32, 7, 777); ++cases; }
        }
    }
    if (test.storage_bindings == 64) {
        for (uint32_t target : {32U, 63U}) {
            const auto pipeline = test.Pipeline(std::filesystem::path{argv[1]} /
                ("high" + std::to_string(target) + ".spv"));
            const uint64_t bit = uint64_t{1} << target;
            test.RunHighBinding(pipeline, target, bit | 1, 1000 + target, 0x13579bdfU);
            test.RunHighBinding(pipeline, target, 1, 0, 0xdeadbeefU);
            test.RunHighBinding(pipeline, target, bit, 0xdeadbeefU, 0x13579bdfU);
            test.RunHighBinding(pipeline, target, (uint64_t{1} << (target == 32 ? 31 : 32)) | 1,
                                0, 0xdeadbeefU);
            cases += 4;
        }
    } else {
        std::cout << "Bindings 32/63 skipped: device cannot bind 64 storage buffers\n";
    }
    std::cout << cases << " synthetic Vulkan buffer-routing cases passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
