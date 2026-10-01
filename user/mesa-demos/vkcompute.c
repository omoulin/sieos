/*
 * vkcompute - Vulkan on SIEOS (Mesa's lavapipe, through the Khronos loader):
 * lists the Vulkan devices, then squares a million numbers with a compute
 * shader (square.comp, compiled to SPIR-V at build time) and checks them.
 *
 *   vkcompute [N]      N numbers (default 1048576)
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

#include "square_spv.h"

#define CHECK(call)                                                                                  \
    do {                                                                                             \
        VkResult r_ = (call);                                                                        \
        if (r_ != VK_SUCCESS) {                                                                      \
            fprintf(stderr, "vkcompute: %s failed: %d\n", #call, r_);                                \
            exit(1);                                                                                 \
        }                                                                                            \
    } while (0)

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "vkcompute: no host-visible memory\n");
    exit(1);
}

static void make_buffer(VkDevice dev, VkPhysicalDevice pd, VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem)
{
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(dev, &bi, NULL, buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, *buf, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = memory_type(pd, req.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(dev, &ai, NULL, mem));
    CHECK(vkBindBufferMemory(dev, *buf, *mem, 0));
}

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1u << 20;
    if (!n) {
        fprintf(stderr, "usage: vkcompute [N]\n");
        return 2;
    }

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkcompute",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ii, NULL, &inst));

    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &count, NULL));
    if (!count) {
        fprintf(stderr, "vkcompute: no Vulkan device\n");
        return 1;
    }
    VkPhysicalDevice pds[8];
    count = count > 8 ? 8 : count;
    CHECK(vkEnumeratePhysicalDevices(inst, &count, pds));
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        printf("device %u: %s, Vulkan %u.%u.%u, driver %u.%u.%u\n", i, p.deviceName,
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
               VK_API_VERSION_PATCH(p.apiVersion), VK_API_VERSION_MAJOR(p.driverVersion),
               VK_API_VERSION_MINOR(p.driverVersion), VK_API_VERSION_PATCH(p.driverVersion));
    }
    VkPhysicalDevice pd = pds[0];

    uint32_t nq = 0, qf = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    nq = nq > 16 ? 16 : nq;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (uint32_t i = 0; i < nq && qf == UINT32_MAX; i++)
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            qf = i;
    if (qf == UINT32_MAX) {
        fprintf(stderr, "vkcompute: no compute queue\n");
        return 1;
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &di, NULL, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    VkDeviceSize size = (VkDeviceSize)n * sizeof(float);
    VkBuffer bin, bout;
    VkDeviceMemory min, mout;
    make_buffer(dev, pd, size, &bin, &min);
    make_buffer(dev, pd, size, &bout, &mout);
    float *in;
    CHECK(vkMapMemory(dev, min, 0, size, 0, (void **)&in));
    for (uint32_t i = 0; i < n; i++)
        in[i] = (float)(i % 1000) * 0.5f;

    VkDescriptorSetLayoutBinding binds[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(uint32_t) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
    VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(square_spv), .pCode = square_spv };
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(dev, &si, NULL, &shader));
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                   .module = shader, .pName = "main" },
        .layout = layout,
    };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                       .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
    VkDescriptorBufferInfo bufs[2] = { { bin, 0, VK_WHOLE_SIZE }, { bout, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet w[2];
    for (int k = 0; k < 2; k++)
        w[k] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                       .dstBinding = (uint32_t)k, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bufs[k] };
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cpool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &bbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(n), &n);
    vkCmdDispatch(cb, (n + 63) / 64, 1, 1);
    CHECK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fi, NULL, &fence));
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    double t0 = now();
    CHECK(vkQueueSubmit(queue, 1, &sub, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    double t1 = now();

    float *out;
    CHECK(vkMapMemory(dev, mout, 0, size, 0, (void **)&out));
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++)
        if (out[i] != in[i] * in[i])
            bad++;
    printf("vkcompute: %u numbers squared on the device in %.1f ms: %s\n", n, (t1 - t0) * 1000,
           bad ? "WRONG" : "all correct");
    if (bad)
        printf("vkcompute: %u wrong (out[1] = %f, expected %f)\n", bad, out[1], in[1] * in[1]);

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, cpool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyShaderModule(dev, shader, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkUnmapMemory(dev, min);
    vkUnmapMemory(dev, mout);
    vkDestroyBuffer(dev, bin, NULL);
    vkDestroyBuffer(dev, bout, NULL);
    vkFreeMemory(dev, min, NULL);
    vkFreeMemory(dev, mout, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return bad ? 1 : 0;
}
