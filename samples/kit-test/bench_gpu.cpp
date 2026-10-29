// kit-test: RADV on GNM through the kit's presenter. Measures what the ports were bound by: the CPU
// time the driver spends per draw (TOTK ~70 us and SSBU ~17 us per draw included the emulators' own
// work; this is the driver alone), the GPU's time per frame, and how evenly frames reach the screen.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ps4_vk_wsi.h"
#include "report.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "quad.frag.h"
#include "quad.vert.h"
#include "quad2.frag.h"

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
    X(vkGetPhysicalDeviceSurfaceSupportKHR)
#define VK_DEVICE_FNS(X)                                                                           \
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
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)
#define DECLARE(fn) PFN_##fn fn;
VK_FNS(DECLARE)
VK_DEVICE_FNS(DECLARE)
#undef DECLARE

struct Frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
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
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
#define LOAD(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(device, #fn);
    VK_DEVICE_FNS(LOAD)
#undef LOAD
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

struct FrameStats {
    uint64_t record_us;  // CPU: recording the draws
    uint64_t submit_us;  // CPU: vkQueueSubmit + vkQueuePresentKHR
    uint64_t wait_us;    // CPU: waiting for the frame two back (GPU-bound when large)
    double gpu_ms;       // GPU: timestamps around the render pass (-1 if unavailable)
    uint64_t present_at_us;
};

// Pipeline switches in the draw-cost test: none, between two pipelines with the same shaders (blend
// differs), between two with different fragment shaders.
enum Switch { kNoSwitch, kSameShaders, kOtherShaders };

// One frame of <draws> quads, binding a pipeline before every draw as <sw> says.
bool frame(uint64_t index, int draws, Switch sw, float r, float g, float b, FrameStats* out) {
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
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[0]);
    const int cols = 100;
    for (int i = 0; i < draws; i++) {
        if (sw != kNoSwitch)
            vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[(i & 1) * (sw == kSameShaders ? 1 : 2)]);
        Push p{};
        p.offset[0] = float(i % cols) / cols;
        p.offset[1] = float((i / cols) % cols) / cols;
        p.size[0] = p.size[1] = 0.8f / cols;
        p.color[0] = float(i & 255) / 255.0f;
        p.color[1] = float((index * 3) & 255) / 255.0f;
        p.color[2] = 0.5f;
        p.color[3] = 0.7f;
        vkCmdPushConstants(f.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof p, &p);
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

// <frames> frames of <draws> draws: the driver's CPU cost per draw.
bool draw_cost(int draws, Switch sw, int nframes, uint64_t* index) {
    std::vector<FrameStats> stats(nframes);
    for (int i = 0; i < nframes; i++)
        if (!frame((*index)++, draws, sw, 0.05f, 0.05f, 0.1f, &stats[i])) return false;
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
    static const char* const kLabels[] = {"                         ", " + switch, same shaders  ",
                                          " + switch, other shaders "};
    report("draw-cost: %5d draws%s: record %.2f us/draw (%.2f ms), submit+present %.2f ms, GPU %.2f ms", draws,
           kLabels[sw], rec / draws, rec / 1000.0, sub / 1000.0, gpu);
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
    uint64_t index = 0;
    bool ok = pacing(300, &index);
    for (int draws : {1000, 5000, 10000})
        ok = ok && draw_cost(draws, kNoSwitch, 40, &index);
    for (Switch sw : {kSameShaders, kOtherShaders})
        for (int draws : {1000, 5000})
            ok = ok && draw_cost(draws, sw, 40, &index);
    // The verdict on screen until the app exits: green, or red when a check failed.
    bool passed = failures() == 0;
    FrameStats s;
    for (int i = 0; ok && i < 20 * 60; i++)
        ok = frame(index++, 0, kNoSwitch, passed ? 0.0f : 0.6f, passed ? 0.5f : 0.0f, 0.0f, &s);
    vkDeviceWaitIdle(device);
    return ok;
}

}  // namespace kittest
