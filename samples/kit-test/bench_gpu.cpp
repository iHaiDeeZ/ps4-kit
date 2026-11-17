// kit-test: RADV on GNM through the kit's presenter. Measures what the ports were bound by: the CPU
// time the driver spends per draw (TOTK ~70 us and SSBU ~17 us per draw included the emulators' own
// work; this is the driver alone), the GPU's time per frame, and how evenly frames reach the screen.
// Besides plain draws and pipeline switches, the draw-cost test times what translation layers (DXVK,
// the Switch and 360 renderers) do around most draws: viewport/scissor and depth/cull state changes,
// vertex and index buffer binds, and descriptors (a bound set with a dynamic offset, push
// descriptors, a set written and bound per draw).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ps4_vk_wsi.h"
#include "report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quad.frag.h"
#include "quad.vert.h"
#include "quad2.frag.h"
#include "quad_ubo.frag.h"
#include "quad_vb.vert.h"

namespace kittest {
namespace {

constexpr uint32_t kWidth = 1920, kHeight = 1080;

PFN_vkGetInstanceProcAddr gipa;
VkInstance instance;
VkPhysicalDevice physical;
VkDevice device;
VkQueue queue;
uint32_t queue_family;

#define VK_FNS(X)                                                                                  \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)             \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)            \
    X(vkCreateHeadlessSurfaceEXT) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)    \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceMemoryProperties)                   \
    X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X)                                                                         \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)         \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImageView)   \
    X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer)       \
    X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule)                       \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines)              \
    X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool)                            \
    X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer)                      \
    X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)        \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkQueueSubmit)     \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)     \
    X(vkDestroySemaphore) X(vkDeviceWaitIdle) X(vkCreateQueryPool) X(vkDestroyQueryPool)           \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkQueueWaitIdle)      \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory)    \
    X(vkMapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)            \
    X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool)                    \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool)                 \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdSetCullMode) X(vkCmdSetFrontFace)       \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp)
#define DECLARE(fn) PFN_##fn fn;
VK_FNS(DECLARE)
VK_DEVICE_FNS(DECLARE)
#undef DECLARE

struct Frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
    VkDescriptorPool set_pool;  // the written-per-draw sets, reset each frame
};

VkSurfaceKHR surface;
VkSwapchainKHR swapchain;
std::vector<VkImageView> views;
std::vector<VkFramebuffer> framebuffers;
VkRenderPass render_pass;
VkPipelineLayout layout;
VkPipeline pipelines[3];
Frame frames[2];
VkQueryPool queries;
float timestamp_period_ns;
bool timestamps_ok;

// The draw-cost test's extra state. One host-visible buffer holds the uniform slots, the vertex
// copies and the index copies; the texture is 4x4. Each descriptor test has its own set layout (a
// dynamic uniform buffer, a plain one, a push descriptor one), so its own pipeline layout and pipeline.
constexpr uint32_t kUboSlots = 256, kVbCopies = 16, kMaxDrawsPerFrame = 10000;
PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
VkBuffer buffer;
VkDeviceSize ubo_align, vb_offset, ib_offset;
VkImageView texture_view;
VkSampler sampler;
VkPipelineLayout layout_dyn, layout_plain, layout_push;
VkDescriptorSetLayout set_layout_plain;
VkDescriptorSet set_dyn;
VkPipeline pipe_dynstate, pipe_vb, pipe_dyn, pipe_plain, pipe_push;

struct Push {
    float offset[2], size[2], color[4];
};

#define VK_CHECK(call)                                                                 \
    do {                                                                               \
        VkResult r_ = (call);                                                          \
        if (r_ != VK_SUCCESS) {                                                        \
            check(false, "vulkan", "%s failed: %d", #call, (int)r_);                   \
            return false;                                                              \
        }                                                                              \
    } while (0)

bool create_device() {
    gipa = ps4kit::vk_get_instance_proc_addr_fn();
    auto create_instance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ps4-kit test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VK_CHECK(create_instance(&ici, nullptr, &instance));
#define LOAD(fn) fn = (PFN_##fn)gipa(instance, #fn);
    VK_FNS(LOAD)
#undef LOAD
    uint32_t count = 1;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    timestamp_period_ns = props.limits.timestampPeriod;
    report("gpu: %s, Vulkan %u.%u.%u, driver 0x%x", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fam.data());
    queue_family = 0;
    for (uint32_t i = 0; i < families; i++)
        if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queue_family = i; break; }
    timestamps_ok = fam[queue_family].timestampValidBits > 0 && timestamp_period_ns > 0;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    uint32_t nexts = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &nexts, nullptr);
    std::vector<VkExtensionProperties> available(nexts);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &nexts, available.data());
    bool push_descriptors = false;
    for (const VkExtensionProperties& e : available)
        push_descriptors |= strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = push_descriptors ? 2 : 1;
    dci.ppEnabledExtensionNames = dexts;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
#define LOAD(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(device, #fn);
    VK_DEVICE_FNS(LOAD)
#undef LOAD
    if (push_descriptors)
        vkCmdPushDescriptorSetKHR =
            (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    return true;
}

bool create_swapchain() {
    VkHeadlessSurfaceCreateInfoEXT hci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VK_CHECK(vkCreateHeadlessSurfaceEXT(instance, &hci, nullptr, &surface));
    uint32_t nformats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nformats, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nformats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nformats, formats.data());
    VkFormat format = nformats ? formats[0].format : VK_FORMAT_R8G8B8A8_UNORM;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = 3;
    sci.imageFormat = format;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = {kWidth, kHeight};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
    uint32_t nimages = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &nimages, nullptr);
    std::vector<VkImage> images(nimages);
    vkGetSwapchainImagesKHR(device, swapchain, &nimages, images.data());

    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &color;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    VK_CHECK(vkCreateRenderPass(device, &rpi, nullptr, &render_pass));

    for (VkImage image : images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &view));
        views.push_back(view);
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = render_pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &view;
        fci.width = kWidth;
        fci.height = kHeight;
        fci.layers = 1;
        VkFramebuffer fb;
        VK_CHECK(vkCreateFramebuffer(device, &fci, nullptr, &fb));
        framebuffers.push_back(fb);
    }
    return true;
}

bool create_pipelines() {
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vs, fs;
    mci.codeSize = sizeof(quad_vert);
    mci.pCode = quad_vert;
    VK_CHECK(vkCreateShaderModule(device, &mci, nullptr, &vs));
    mci.codeSize = sizeof(quad_frag);
    mci.pCode = quad_frag;
    VK_CHECK(vkCreateShaderModule(device, &mci, nullptr, &fs));

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(device, &lci, nullptr, &layout));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = layout;
    pci.renderPass = render_pass;
    // Two pipelines that differ in blending: switching between them is a real state change.
    uint64_t t0 = now_us();
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipelines[0]));
    uint64_t first_us = now_us() - t0;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipelines[1]));
    report("pipelines: first %.2f ms (ACO compile), second %.2f ms", first_us / 1000.0,
           (now_us() - t0 - first_us) / 1000.0);
    // A third with pipeline 0's state and another fragment shader: a switch that changes shaders.
    VkShaderModule fs2;
    mci.codeSize = sizeof(quad2_frag);
    mci.pCode = quad2_frag;
    VK_CHECK(vkCreateShaderModule(device, &mci, nullptr, &fs2));
    stages[1].module = fs2;
    blend.blendEnable = VK_FALSE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipelines[2]));
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    vkDestroyShaderModule(device, fs2, nullptr);

    for (Frame& f : frames) {
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = queue_family;
        VK_CHECK(vkCreateCommandPool(device, &cpi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = f.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(device, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &f.acquired));
        VK_CHECK(vkCreateSemaphore(device, &sci, nullptr, &f.rendered));
    }
    if (timestamps_ok) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 4;  // 2 per frame in flight
        timestamps_ok = vkCreateQueryPool(device, &qpi, nullptr, &queries) == VK_SUCCESS;
    }
    return true;
}

int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
}

// A pipeline for the draw-cost test's extra cases: the quad shaders (corners from a vertex buffer
// when <vertex_input>), <fs>, opaque, depth/cull state dynamic when <dynamic_depth_cull>.
bool make_pipeline(VkPipelineLayout pl, bool vertex_input, const uint32_t* fs_code, size_t fs_size,
                   bool dynamic_depth_cull, VkPipeline* out) {
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vs, fs;
    mci.codeSize = vertex_input ? sizeof(quad_vb_vert) : sizeof(quad_vert);
    mci.pCode = vertex_input ? quad_vb_vert : quad_vert;
    VK_CHECK(vkCreateShaderModule(device, &mci, nullptr, &vs));
    mci.codeSize = fs_size;
    mci.pCode = fs_code;
    VK_CHECK(vkCreateShaderModule(device, &mci, nullptr, &fs));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkVertexInputBindingDescription vb{0, 2 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (vertex_input) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 1;
        vi.pVertexAttributeDescriptions = &va;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_CULL_MODE,         VK_DYNAMIC_STATE_FRONT_FACE,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = dynamic_depth_cull ? 7 : 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &dss;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = pl;
    pci.renderPass = render_pass;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, out));
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    return true;
}

// The buffer, texture, descriptor layouts and pipelines of the draw-cost test's extra cases.
bool create_draw_state() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    ubo_align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
    vb_offset = kUboSlots * ubo_align;
    ib_offset = vb_offset + kVbCopies * 4 * 2 * sizeof(float);
    const VkDeviceSize size = ib_offset + 2 * 16;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) { check(false, "vulkan", "no host-visible memory for the test buffer"); return false; }
    mai.memoryTypeIndex = uint32_t(type);
    VkDeviceMemory buffer_memory;
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &buffer_memory));
    VK_CHECK(vkBindBufferMemory(device, buffer, buffer_memory, 0));
    char* data;
    VK_CHECK(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void**)&data));
    for (uint32_t s = 0; s < kUboSlots; s++) {
        float tint[4] = {1.0f, 1.0f - s / 512.0f, 1.0f, 1.0f};
        memcpy(data + s * ubo_align, tint, sizeof tint);
    }
    static const float kCorners[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    for (uint32_t c = 0; c < kVbCopies; c++) memcpy(data + vb_offset + c * sizeof kCorners, kCorners, sizeof kCorners);
    static const uint16_t kIndices[6] = {0, 1, 2, 1, 3, 2};
    for (int c = 0; c < 2; c++) memcpy(data + ib_offset + c * 16, kIndices, sizeof kIndices);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {4, 4, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage texture;
    VK_CHECK(vkCreateImage(device, &ici, nullptr, &texture));
    vkGetImageMemoryRequirements(device, texture, &req);
    mai.allocationSize = req.size;
    type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = memory_type(req.memoryTypeBits, 0);
    mai.memoryTypeIndex = uint32_t(type);
    VkDeviceMemory texture_memory;
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &texture_memory));
    VK_CHECK(vkBindImageMemory(device, texture, texture_memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = texture;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device, &vci, nullptr, &texture_view));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(device, &sci, nullptr, &sampler));

    // The texture's contents (white) and layout, once, on frame 0's command buffer.
    VkCommandBuffer cmd = frames[0].cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture;
    barrier.subresourceRange = vci.subresourceRange;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &vci.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    // Set layouts: binding 0 the uniform buffer (dynamic in the bound-set case), binding 1 the texture.
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    auto make_layout = [&](VkDescriptorType ubo_type, VkDescriptorSetLayoutCreateFlags flags,
                           VkDescriptorSetLayout* set_layout, VkPipelineLayout* pl) {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0] = {0, ubo_type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.flags = flags;
        dli.bindingCount = 2;
        dli.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &dli, nullptr, set_layout));
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = 1;
        lci.pSetLayouts = set_layout;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;
        VK_CHECK(vkCreatePipelineLayout(device, &lci, nullptr, pl));
        return true;
    };
    VkDescriptorSetLayout sl_dyn, sl_push;
    if (!make_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0, &sl_dyn, &layout_dyn) ||
        !make_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &set_layout_plain, &layout_plain))
        return false;
    if (vkCmdPushDescriptorSetKHR &&
        !make_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                     &sl_push, &layout_push))
        return false;

    // The bound set (written once) and each frame's pool for the per-draw sets.
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool_dyn;
    VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &pool_dyn));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool_dyn;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &sl_dyn;
    VK_CHECK(vkAllocateDescriptorSets(device, &dai, &set_dyn));
    VkDescriptorBufferInfo ubo{buffer, 0, 16};
    VkDescriptorImageInfo tex{sampler, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set_dyn, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            nullptr, &ubo, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set_dyn, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &tex, nullptr, nullptr};
    vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDrawsPerFrame};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxDrawsPerFrame};
    dpi.maxSets = kMaxDrawsPerFrame;
    for (Frame& f : frames) VK_CHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &f.set_pool));

    return make_pipeline(layout, false, quad_frag, sizeof(quad_frag), true, &pipe_dynstate) &&
           make_pipeline(layout, true, quad_frag, sizeof(quad_frag), false, &pipe_vb) &&
           make_pipeline(layout_dyn, false, quad_ubo_frag, sizeof(quad_ubo_frag), false, &pipe_dyn) &&
           make_pipeline(layout_plain, false, quad_ubo_frag, sizeof(quad_ubo_frag), false, &pipe_plain) &&
           (!vkCmdPushDescriptorSetKHR ||
            make_pipeline(layout_push, false, quad_ubo_frag, sizeof(quad_ubo_frag), false, &pipe_push));
}

struct FrameStats {
    uint64_t record_us;  // CPU: recording the draws
    uint64_t submit_us;  // CPU: vkQueueSubmit + vkQueuePresentKHR
    uint64_t wait_us;    // CPU: waiting for the frame two back (GPU-bound when large)
    double gpu_ms;       // GPU: timestamps around the render pass (-1 if unavailable)
    uint64_t present_at_us;
};

// What the draw-cost test does before every draw, besides its push constants: nothing; a pipeline
// switch between two with the same shaders (blend differs) or with different fragment shaders;
// viewport + scissor; cull mode, front face and depth test/write/compare; a vertex and an index
// buffer bind (indexed draw); the bound set again with another dynamic uniform offset; push
// descriptors (uniform buffer + texture); a new set from the frame's pool written and bound. Values
// alternate, so the driver can't skip them as unchanged.
enum Mode {
    kNoSwitch,
    kSameShaders,
    kOtherShaders,
    kViewport,
    kDepthCull,
    kVertexIndex,
    kBindSet,
    kPushDescriptors,
    kWriteSet,
    kModes
};

// One frame of <draws> quads, with <mode>'s work before every draw.
bool frame(uint64_t index, int draws, Mode mode, float r, float g, float b, FrameStats* out) {
    Frame& f = frames[index & 1];
    uint64_t t0 = now_us();
    vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    uint64_t t1 = now_us();
    out->gpu_ms = -1;
    if (timestamps_ok && index >= 2) {
        uint64_t ts[2];
        if (vkGetQueryPoolResults(device, queries, (index & 1) * 2, 2, sizeof ts, ts, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            out->gpu_ms = double(ts[1] - ts[0]) * timestamp_period_ns / 1e6;
    }
    vkResetFences(device, 1, &f.fence);
    if (mode == kWriteSet) vkResetDescriptorPool(device, f.set_pool, 0);
    uint32_t image;
    VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        check(false, "vulkan", "vkAcquireNextImageKHR: %d", (int)acq);
        return false;
    }

    uint64_t t2 = now_us();
    vkResetCommandPool(device, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    if (timestamps_ok) {
        vkCmdResetQueryPool(f.cmd, queries, (index & 1) * 2, 2);
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, (index & 1) * 2);
    }
    VkClearValue clear{};
    clear.color = {{r, g, b, 1.0f}};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = render_pass;
    rbi.framebuffer = framebuffers[image];
    rbi.renderArea = {{0, 0}, {kWidth, kHeight}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    vkCmdSetViewport(f.cmd, 0, 1, &viewport);
    vkCmdSetScissor(f.cmd, 0, 1, &scissor);
    VkPipeline first = pipelines[0];
    VkPipelineLayout pl = layout;
    switch (mode) {
    case kDepthCull: first = pipe_dynstate; break;
    case kVertexIndex: first = pipe_vb; break;
    case kBindSet: first = pipe_dyn, pl = layout_dyn; break;
    case kPushDescriptors: first = pipe_push, pl = layout_push; break;
    case kWriteSet: first = pipe_plain, pl = layout_plain; break;
    default: break;
    }
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, first);
    const int cols = 100;
    for (int i = 0; i < draws; i++) {
        const bool odd = i & 1;
        switch (mode) {
        case kNoSwitch: break;
        case kSameShaders:
        case kOtherShaders:
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipelines[odd * (mode == kSameShaders ? 1 : 2)]);
            break;
        case kViewport: {
            VkViewport v{odd ? 4.0f : 0.0f, 0, float(kWidth), float(kHeight), 0, 1};
            VkRect2D s{{0, 0}, {kWidth - (odd ? 4u : 0u), kHeight}};
            vkCmdSetViewport(f.cmd, 0, 1, &v);
            vkCmdSetScissor(f.cmd, 0, 1, &s);
            break;
        }
        case kDepthCull:
            vkCmdSetCullMode(f.cmd, odd ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
            vkCmdSetFrontFace(f.cmd, odd ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            vkCmdSetDepthTestEnable(f.cmd, odd);
            vkCmdSetDepthWriteEnable(f.cmd, odd);
            vkCmdSetDepthCompareOp(f.cmd, odd ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS);
            break;
        case kVertexIndex: {
            VkDeviceSize vb = vb_offset + (i % kVbCopies) * 4 * 2 * sizeof(float);
            vkCmdBindVertexBuffers(f.cmd, 0, 1, &buffer, &vb);
            vkCmdBindIndexBuffer(f.cmd, buffer, ib_offset + (odd ? 16 : 0), VK_INDEX_TYPE_UINT16);
            break;
        }
        case kBindSet: {
            uint32_t offset = uint32_t((i % kUboSlots) * ubo_align);
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_dyn, 0, 1, &set_dyn, 1, &offset);
            break;
        }
        case kPushDescriptors:
        case kWriteSet: {
            VkDescriptorBufferInfo ubo{buffer, (i % kUboSlots) * ubo_align, 16};
            VkDescriptorImageInfo tex{sampler, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (mode == kWriteSet) {
                VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                dai.descriptorPool = f.set_pool;
                dai.descriptorSetCount = 1;
                dai.pSetLayouts = &set_layout_plain;
                if (vkAllocateDescriptorSets(device, &dai, &set) != VK_SUCCESS) break;
            }
            VkWriteDescriptorSet w[2]{};
            w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    nullptr, &ubo, nullptr};
            w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &tex, nullptr, nullptr};
            if (mode == kPushDescriptors) {
                vkCmdPushDescriptorSetKHR(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_push, 0, 2, w);
            } else {
                vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
                vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_plain, 0, 1, &set, 0, nullptr);
            }
            break;
        }
        case kModes: break;
        }
        Push p{};
        p.offset[0] = float(i % cols) / cols;
        p.offset[1] = float((i / cols) % cols) / cols;
        p.size[0] = p.size[1] = 0.8f / cols;
        p.color[0] = float(i & 255) / 255.0f;
        p.color[1] = float((index * 3) & 255) / 255.0f;
        p.color[2] = 0.5f;
        p.color[3] = 0.7f;
        vkCmdPushConstants(f.cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof p, &p);
        if (mode == kVertexIndex)
            vkCmdDrawIndexed(f.cmd, 6, 1, 0, 0, 0);
        else
            vkCmdDraw(f.cmd, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(f.cmd);
    if (timestamps_ok)
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, (index & 1) * 2 + 1);
    vkEndCommandBuffer(f.cmd);
    uint64_t t3 = now_us();

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &f.rendered;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, f.fence));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &f.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &image;
    VkResult pr = vkQueuePresentKHR(queue, &pi);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
        check(false, "vulkan", "vkQueuePresentKHR: %d", (int)pr);
        return false;
    }
    uint64_t t4 = now_us();
    out->wait_us = t1 - t0;
    out->record_us = t3 - t2;
    out->submit_us = t4 - t3;
    out->present_at_us = t4;
    return true;
}

// <frames> frames of <draws> draws: the driver's CPU cost per draw. <plain_us>: the plain draw's cost
// at this count, for the extra cost of <mode>'s work (0: not known yet; set from a kNoSwitch run).
bool draw_cost(int draws, Mode mode, int nframes, uint64_t* index, double* plain_us = nullptr) {
    std::vector<FrameStats> stats(nframes);
    for (int i = 0; i < nframes; i++)
        if (!frame((*index)++, draws, mode, 0.05f, 0.05f, 0.1f, &stats[i])) return false;
    // Skip the first frames (first-use allocations), take medians.
    auto median = [&](auto field) {
        std::vector<double> v;
        for (int i = 4; i < nframes; i++) v.push_back(field(stats[i]));
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    double rec = median([](const FrameStats& s) { return double(s.record_us); });
    double sub = median([](const FrameStats& s) { return double(s.submit_us); });
    double gpu = median([](const FrameStats& s) { return s.gpu_ms; });
    static const char* const kLabels[kModes] = {
        "                          ", " + switch, same shaders   ", " + switch, other shaders  ",
        " + viewport, scissor      ", " + cull, depth state      ", " + vertex/index bind      ",
        " + set, dynamic offset    ", " + push descriptors       ", " + set written + bound    "};
    const double per_draw = rec / draws;
    char extra[48] = "";
    if (plain_us && mode == kNoSwitch)
        *plain_us = per_draw;
    else if (plain_us && *plain_us > 0)
        snprintf(extra, sizeof extra, " (+%.2f us)", per_draw - *plain_us);
    report("draw-cost: %5d draws%s: record %.2f us/draw%s (%.2f ms), submit+present %.2f ms, GPU %.2f ms", draws,
           kLabels[mode], per_draw, extra, rec / 1000.0, sub / 1000.0, gpu);
    return true;
}

// How evenly frames reach the screen: <nframes> light frames under FIFO, intervals between presents.
// At 60 Hz every interval should be ~16.7 ms; a long one is a missed vblank (a visible hitch).
bool pacing(int nframes, uint64_t* index) {
    std::vector<FrameStats> stats(nframes);
    uint64_t flips0 = ps4kit::flip_count();
    for (int i = 0; i < nframes; i++)
        if (!frame((*index)++, 200, kNoSwitch, 0.05f, 0.05f, 0.1f, &stats[i])) return false;
    std::vector<double> gaps;
    for (int i = 11; i < nframes; i++) gaps.push_back((stats[i].present_at_us - stats[i - 1].present_at_us) / 1000.0);
    double sum = 0, sq = 0, worst = 0;
    int long_gaps = 0;
    for (double g : gaps) {
        sum += g;
        sq += g * g;
        worst = std::max(worst, g);
        if (g > 16.7 * 1.5) long_gaps++;
    }
    double mean = sum / gaps.size(), sd = std::sqrt(std::max(0.0, sq / gaps.size() - mean * mean));
    uint64_t flips = ps4kit::flip_count() - flips0;
    check(long_gaps <= 2 && mean > 14 && mean < 19, "frame-pacing",
          "%zu frames: %.2f ms mean (%.1f fps), sd %.2f ms, worst %.2f ms, %d late; %llu flips", gaps.size(), mean,
          1000.0 / mean, sd, worst, long_gaps, (unsigned long long)flips);
    return true;
}

}  // namespace

bool run_gpu_tests() {
    report("== GPU");
    uint64_t t0 = now_us();
    if (!create_device() || !create_swapchain() || !create_pipelines()) return false;
    report("vulkan up in %.1f ms", (now_us() - t0) / 1000.0);
    if (!create_draw_state()) return false;
    uint64_t index = 0;
    bool ok = pacing(300, &index);
    double plain_5000 = 0;
    for (int draws : {1000, 5000, 10000})
        ok = ok && draw_cost(draws, kNoSwitch, 40, &index, draws == 5000 ? &plain_5000 : nullptr);
    for (Mode mode : {kSameShaders, kOtherShaders})
        for (int draws : {1000, 5000})
            ok = ok && draw_cost(draws, mode, 40, &index, draws == 5000 ? &plain_5000 : nullptr);
    // What translation layers do around a draw, 5000 draws each; "+" is the cost over a plain draw.
    for (Mode mode : {kViewport, kDepthCull, kVertexIndex, kBindSet, kPushDescriptors, kWriteSet}) {
        if (mode == kPushDescriptors && !vkCmdPushDescriptorSetKHR) {
            report("draw-cost: push descriptors skipped (no VK_KHR_push_descriptor)");
            continue;
        }
        ok = ok && draw_cost(5000, mode, 40, &index, &plain_5000);
    }
    // The verdict on screen until the app exits: green, or red when a check failed.
    bool passed = failures() == 0;
    FrameStats s;
    for (int i = 0; ok && i < 20 * 60; i++)
        ok = frame(index++, 0, kNoSwitch, passed ? 0.0f : 0.6f, passed ? 0.5f : 0.0f, 0.0f, &s);
    vkDeviceWaitIdle(device);
    return ok;
}

}  // namespace kittest
