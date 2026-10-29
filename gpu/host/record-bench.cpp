// record-bench: the CPU cost of recording draws in the kit's RADV, measured and profiled on the PC.
//
// Loads the host build of the kit's RADV (gpu/host/build-mesa-host.sh) against drm-shim with the
// PS4's GPU identity, records the same command buffers kit-test does on the console (quads placed by
// push constants, with and without a pipeline switch before every draw), never submits, and samples
// itself with SIGPROF meanwhile: each sample's stack (frame pointers) goes to a file of
// <module offset> lines that gpu/host/profile-report.py turns into a function profile.
//
//   LD_PRELOAD=<mesa-host>/src/amd/drm-shim/libamdgpu_noop_drm_shim.so AMDGPU_GPU_ID=bonaire \
//     record-bench <libvulkan_radeon.so> <samples-out> [draws] [repeats]
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "quad.frag.h"
#include "quad.vert.h"
#include "quad2.frag.h"

namespace {

// --- Sampler -----------------------------------------------------------------------------------
constexpr int kDepth = 24;
constexpr size_t kMaxSamples = 400000;
struct Sample {
    uint8_t depth;
    void* pc[kDepth];
};
Sample* g_samples;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_sampling{false};

void on_prof(int, siginfo_t*, void* uc_) {
    if (!g_sampling.load(std::memory_order_relaxed)) return;
    size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    auto* uc = static_cast<ucontext_t*>(uc_);
    Sample& s = g_samples[i];
    s.pc[0] = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
    auto* fp = reinterpret_cast<void**>(uc->uc_mcontext.gregs[REG_RBP]);
    int d = 1;
    // Leaf functions may not have set up a frame yet: the caller is then at [rsp].
    s.pc[d++] = *reinterpret_cast<void**>(uc->uc_mcontext.gregs[REG_RSP]);
    while (d < kDepth && fp && (reinterpret_cast<uintptr_t>(fp) & 7) == 0) {
        void** next = static_cast<void**>(fp[0]);
        void* ret = fp[1];
        if (!ret || next <= fp || next - fp > (1 << 20)) break;
        s.pc[d++] = ret;
        fp = next;
    }
    s.depth = static_cast<uint8_t>(d);
}

void start_sampling() {
    struct sigaction sa{};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval t{{0, 100}, {0, 100}};  // every 100 us of CPU time
    setitimer(ITIMER_PROF, &t, nullptr);
    g_sampling = true;
}

void stop_sampling() {
    g_sampling = false;
    itimerval t{};
    setitimer(ITIMER_PROF, &t, nullptr);
}

// Writes "<module>\t<offset hex>" per frame, a blank line between samples.
void dump_samples(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    size_t n = std::min(g_count.load(), kMaxSamples);
    for (size_t i = 0; i < n; i++) {
        for (int d = 0; d < g_samples[i].depth; d++) {
            Dl_info info;
            void* pc = g_samples[i].pc[d];
            // Return addresses point after the call: step back into it for the symbolizer.
            void* lookup = d ? static_cast<char*>(pc) - 1 : pc;
            if (dladdr(lookup, &info) && info.dli_fname)
                fprintf(f, "%s\t%lx\n", info.dli_fname,
                        (unsigned long)(static_cast<char*>(lookup) - static_cast<char*>(info.dli_fbase)));
            else
                fprintf(f, "?\t%p\n", pc);
        }
        fputc('\n', f);
    }
    fclose(f);
    printf("%zu samples -> %s\n", n, path);
}

// --- Vulkan ------------------------------------------------------------------------------------
PFN_vkGetInstanceProcAddr gipa;
VkInstance instance;
VkPhysicalDevice physical;
VkDevice device;
#define FNS(X)                                                                                   \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkCreateDevice)              \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties)
#define DFNS(X)                                                                                  \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory)    \
    X(vkCreateImageView) X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule)    \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateCommandPool)                \
    X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdPushConstants)      \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor)
#define DECL(fn) PFN_##fn fn;
FNS(DECL)
DFNS(DECL)

VkRenderPass rp;
VkFramebuffer fb;
VkPipelineLayout layout;
VkPipeline pipes[3];
VkCommandPool pool;
VkCommandBuffer cmd;

struct Push {
    float offset[2], size[2], color[4];
};

#define CHECK(x)                                                    \
    do {                                                            \
        VkResult r_ = (x);                                          \
        if (r_) { fprintf(stderr, "%s = %d\n", #x, r_); exit(1); }  \
    } while (0)

void setup(const char* icd) {
    void* lib = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); exit(1); }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    auto ci = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(ci(&ici, nullptr, &instance));
#define LOAD(fn) fn = (PFN_##fn)gipa(instance, #fn);
    FNS(LOAD)
#undef LOAD
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &physical));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    printf("device: %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
#define LOAD(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(device, #fn);
    DFNS(LOAD)
#undef LOAD

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {1920, 1080, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage image;
    CHECK(vkCreateImage(device, &ii, nullptr, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    uint32_t type = 0;
    while (!(req.memoryTypeBits & (1u << type))) type++;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
    CHECK(vkBindImageMemory(device, image, mem, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ii.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    CHECK(vkCreateImageView(device, &vci, nullptr, &view));

    VkAttachmentDescription color{};
    color.format = ii.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &color;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    CHECK(vkCreateRenderPass(device, &rpi, nullptr, &rp));
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &view;
    fci.width = 1920;
    fci.height = 1080;
    fci.layers = 1;
    CHECK(vkCreateFramebuffer(device, &fci, nullptr, &fb));

    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = sizeof quad_vert;
    mci.pCode = quad_vert;
    CHECK(vkCreateShaderModule(device, &mci, nullptr, &vs));
    mci.codeSize = sizeof quad_frag;
    mci.pCode = quad_frag;
    CHECK(vkCreateShaderModule(device, &mci, nullptr, &fs));
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    CHECK(vkCreatePipelineLayout(device, &lci, nullptr, &layout));
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1;
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
    pci.pStages = st;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = layout;
    pci.renderPass = rp;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipes[0]));
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipes[1]));
    // pipes[0]'s state with another fragment shader: a switch that changes shaders.
    VkShaderModule fs2;
    mci.codeSize = sizeof quad2_frag;
    mci.pCode = quad2_frag;
    CHECK(vkCreateShaderModule(device, &mci, nullptr, &fs2));
    st[1].module = fs2;
    blend.blendEnable = VK_FALSE;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipes[2]));

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    CHECK(vkCreateCommandPool(device, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
}

// Records one command buffer; returns the recording time in microseconds. sw: 0 no pipeline switch,
// 1 switches between pipelines with the same shaders, 2 between pipelines with other shaders.
double record(int draws, int sw) {
    auto t0 = std::chrono::steady_clock::now();
    vkResetCommandPool(device, pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear{};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = {{0, 0}, {1920, 1080}};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, 1920, 1080, 0, 1};
    VkRect2D s{{0, 0}, {1920, 1080}};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &s);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[0]);
    for (int i = 0; i < draws; i++) {
        if (sw) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[(i & 1) * sw]);
        Push p{};
        p.offset[0] = float(i % 100) / 100;
        p.offset[1] = float((i / 100) % 100) / 100;
        p.size[0] = p.size[1] = 0.008f;
        p.color[0] = float(i & 255) / 255;
        p.color[3] = 0.7f;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

double median_us_per_draw(int draws, int sw, int repeats) {
    std::vector<double> t;
    for (int r = 0; r < repeats; r++) t.push_back(record(draws, sw) / draws);
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: record-bench <libvulkan_radeon.so> <samples-out> [draws] [repeats]\n");
        return 2;
    }
    int draws = argc > 3 ? atoi(argv[3]) : 5000;
    int repeats = argc > 4 ? atoi(argv[4]) : 200;
    g_samples = static_cast<Sample*>(calloc(kMaxSamples, sizeof(Sample)));
    setup(argv[1]);
    record(draws, 2);  // warm up
    double plain = median_us_per_draw(draws, 0, repeats);
    double other = median_us_per_draw(draws, 2, repeats / 3);
    start_sampling();
    double same = median_us_per_draw(draws, 1, repeats);
    stop_sampling();
    printf("%d draws: %.3f us/draw plain; per switch: %.3f us with the same shaders, %.3f us with other shaders\n",
           draws, plain, same - plain, other - plain);
    dump_samples(argv[2]);
    return 0;
}
