// ps4-kit: Vulkan window system integration on VideoOut (ps4_vk_wsi.cpp).
//
// The statically linked RADV has no WSI. ps4_vk_wsi.cpp adds VK_EXT_headless_surface (+ VK_KHR_surface) and
// VK_KHR_swapchain on top of it; an app (DXVK, volk, vulkan.hpp's dynamic dispatcher, a plain renderer) loads every
// Vulkan function through vk_get_instance_proc_addr() below instead of a loader, creates a headless surface and uses
// the standard swapchain calls. Presenting queues a VideoOut flip behind the GPU work (no CPU wait).
//
// What an app must know:
// - The screen is 1920x1080 (the system scales it for a 720p TV). Formats: R8G8B8A8_UNORM or B8G8R8A8_UNORM.
// - Binary semaphores are dropped from every submit (they lose the device on the PS4); timeline semaphores are kept.
//   There is one in-order queue, so the acquire/present semaphores aren't needed for ordering.
// - VideoOut buffer slots 0-11 belong to the swapchains; 12-15 are free for the app's own screens (a launcher or boot
//   screen that writes VideoOut buffers itself). Such an app flips with its own flipArg range (HENkaku's launcher:
//   0x4c000000 and up), so it can't be mistaken for a swapchain flip number.
#pragma once

#include <cstdint>

namespace ps4kit {

// VideoOut's 1080p output.
constexpr uint32_t kScreenWidth = 1920;
constexpr uint32_t kScreenHeight = 1080;

// VideoOut buffer slots: the swapchains take 0-11 (3 banks of 4), the app may register 12-15 itself.
constexpr int kAppFirstBuffer = 12;

// --- Vulkan ------------------------------------------------------------------------------------------------------

// vkGetInstanceProcAddr of the statically linked RADV with the PS4 surface and swapchain added. A
// PFN_vkGetInstanceProcAddr (void* so this header needs no Vulkan headers; see the typed one below).
void* vk_get_instance_proc_addr();

// --- VideoOut ----------------------------------------------------------------------------------------------------

// Opens VideoOut and its flip event queue (once; the first swapchain does it too). Call it early on the main thread:
// an app's own boot screen needs the handle before any swapchain exists, and the step trace then shows a failing
// sceVideoOutOpen on its own line. False when VideoOut can't be opened.
bool video_out_open();

// The VideoOut handle (opened on first use), or -1. For an app that registers its own buffers in slots 12-15.
int video_handle();

// --- Flips and frame pacing --------------------------------------------------------------------------------------

// Flips submitted by swapchains so far. Flip n is submitted with flipArg n, so it can be compared with
// FlipInfo::shown. (The first one hides the system's splash screen.)
uint64_t flip_count();

// VideoOut's flip status right now (sceVideoOutGetFlipStatus), with the swapchain side's counts.
struct FlipInfo {
    uint64_t submitted = 0;      // flips submitted (flip_count())
    int64_t shown = 0;           // flipArg of the last flip VideoOut has made (= its flip number for ours)
    uint64_t flips_made = 0;     // VideoOut's own count of flips made (any flipper)
    uint64_t flip_time = 0;      // when the last flip was made: VideoOut's ptime, unconverted (see the notes)
    int32_t gpu_pending = 0;     // flips queued behind GPU work, not made yet
    int32_t pending = 0;         // flips waiting for their vblank
    int32_t current_buffer = -1; // the buffer slot on screen
    uint64_t last_present_ns = 0;  // steady_clock time of the last present, ns
};
// False (info left zeroed) when VideoOut isn't open or the call fails.
bool flip_info(FlipInfo* info);

// Whether the last flip submitted still waits for its vblank after up to grace_us (polled every 200 us). For frame
// generation: a render thread that would otherwise wait a whole vblank for a free swapchain image skips that
// vblank's in-between image instead. False when nothing is pending (or VideoOut isn't open).
bool flip_pending(unsigned grace_us);

// Waits for the display's next vblank (its own event queue, made on first use). True at a vblank; false on a timeout
// or when vblank events can't be had, and after the first timeout it answers false at once for good: a caller that
// paces itself on vblanks then keeps its own time instead of slowing down. One thread at a time.
bool wait_vblank(unsigned timeout_us);

// Vblanks a second as measured by wait_vblank() over its first 120 vblanks (0 until then). 59.94 on a 60 Hz output.
double vblank_rate();

// The last 10 s window of presents (also logged at the end of each window when stats logging is on).
struct PresentStats {
    double flips_per_second = 0;
    double acquire_wait_ms_per_second = 0;  // time vkAcquireNextImageKHR waited for its image to leave the screen
    double longest_frame_ms = 0;            // longest time between two presents (hitches)
    uint32_t acquire_timeouts = 0;          // acquires that gave up waiting (200 ms) and used the image anyway
};
PresentStats present_stats();

// --- Presenting into an app's own windows ---------------------------------------------------------------------------

// A presenter in place of VideoOut, for an app that shows the frames itself (WinePS4: in a program's window on its
// desktop, whose process doesn't have VideoOut). Once set, swapchains are made at the size asked for (1 to 4096 a
// side), their images linear in host-visible memory the CPU reads (cached where there is such a type), and a present
// waits for the GPU, then hands each image to the hook: the surface the swapchain was made for, the pixels (row_bytes
// apart), the size, and whether it is B8G8R8A8 (else R8G8B8A8). The pixels stay until the image is acquired again.
// The surfaces' capabilities then take any size. Set it before the first swapchain is made.
using WindowPresent = void (*)(uint64_t surface, const void* pixels, uint32_t width, uint32_t height,
                               uint32_t row_bytes, bool bgra);
void set_window_present(WindowPresent hook);

// --- Options (all may change at any time) --------------------------------------------------------------------------

// Where the WSI's log lines go. Default (nullptr): ps4_boot_trace() of the kit's ps4_crashlog.cpp when it is
// linked, else nowhere.
void set_log(void (*log)(const char* line));

// Lines of step trace (each Vulkan WSI call and its result, written straight to ps4_boot_trace so the last line
// before a crash without crash.log names the call). Default 400 (the first frames); 0 turns it off.
void set_step_trace(int lines);

// The 10 s present statistics in the log (default on). present_stats() works either way.
void set_stats_log(bool on);

// Flip every swapchain at vsync, IMMEDIATE ones too (default off). For frame generation, which presents its
// in-between image right behind the real one: an hsync flip would show each only for part of a scan.
void set_force_vsync(bool on);

}  // namespace ps4kit

// The typed vkGetInstanceProcAddr, when the Vulkan headers are included before this one.
#ifdef VULKAN_CORE_H_
namespace ps4kit {
inline PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr_fn() {
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_get_instance_proc_addr());
}
}  // namespace ps4kit
#endif

// C entry points (C code, and DXVK's patched src/vulkan/vulkan_loader.cpp, which looks up
// dxvk_ps4_vk_get_instance_proc_addr; that one is weak so an app may define its own).
extern "C" {
void* ps4kit_vk_get_instance_proc_addr(void);
// set_log() for C
void ps4kit_set_log(void (*log)(const char* line));
// set_window_present() for C (bgra: 1 for B8G8R8A8, 0 for R8G8B8A8)
void ps4kit_set_window_present(void (*hook)(unsigned long long surface, const void* pixels, unsigned width,
                                            unsigned height, unsigned row_bytes, int bgra));
int ps4kit_video_handle(void);
unsigned long long ps4kit_flip_count(void);
}
