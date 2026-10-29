// kit-test: the runtime's CPU-side pieces under the loads that broke them in the ports.
#include "report.h"

#include "ps4_kit.h"

#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

// libkernel, declared by hand (the OpenOrbis headers clash with libc++'s).
extern "C" {
int scePthreadSetprio(pthread_t thread, int prio);
int scePthreadSetaffinity(pthread_t thread, uint64_t mask);
int sceKernelGetCurrentCpu(void);
}

namespace kittest {
namespace {

// --- malloc under mixed priorities -------------------------------------------------------------
// dlmalloc's default x86 lock spins and sched_yield()s, and the PS4's scheduler only yields to threads
// of the same or higher priority: a low-priority thread holding the lock never ran again while
// higher-priority ones spun for it (Crema 1.61, CBFD v00.09/v00.10, TOTK froze that way). With the
// kit's blocking locks this finishes; with spin locks it livelocks and the watchdog reports it.
struct MallocLoad {
    int prio;
    int iterations;
    std::atomic<int>* done;
    uint64_t elapsed_us;
};

void* malloc_worker(void* arg) {
    auto* load = static_cast<MallocLoad*>(arg);
    scePthreadSetprio(pthread_self(), load->prio);
    // All on one core: the livelock needs the holder and the spinners to compete for the same core.
    scePthreadSetaffinity(pthread_self(), 1u << 4);
    uint64_t t0 = now_us();
    void* keep[64] = {};
    unsigned seed = static_cast<unsigned>(load->prio);
    for (int i = 0; i < load->iterations; i++) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) & 63;
        free(keep[slot]);
        size_t size = 16 + ((seed >> 16) & 4095);
        keep[slot] = malloc(size);
        if (keep[slot]) memset(keep[slot], slot, 16);
    }
    for (void* p : keep) free(p);
    load->elapsed_us = now_us() - t0;
    load->done->fetch_add(1);
    return nullptr;
}

void test_malloc_priorities() {
    constexpr int kThreads = 6;
    constexpr int kIterations = 200000;
    std::atomic<int> done{0};
    MallocLoad loads[kThreads];
    pthread_t threads[kThreads];
    for (int i = 0; i < kThreads; i++) {
        // PS4 priorities: 256 (highest) .. 767 (lowest). Half high, half low.
        loads[i] = {i < kThreads / 2 ? 300 : 767, kIterations, &done, 0};
        pthread_create(&threads[i], nullptr, malloc_worker, &loads[i]);
    }
    uint64_t t0 = now_us();
    while (done.load() < kThreads && now_us() - t0 < 20'000'000) {
        struct timespec ts{0, 10'000'000};
        nanosleep(&ts, nullptr);
    }
    bool finished = done.load() == kThreads;
    if (!finished) {
        check(false, "malloc-priorities", "%d of %d threads still stuck after 20 s (spin-lock livelock)",
              kThreads - done.load(), kThreads);
        return;  // the threads are left running: joining would hang
    }
    uint64_t worst = 0;
    for (int i = 0; i < kThreads; i++) {
        pthread_join(threads[i], nullptr);
        if (loads[i].elapsed_us > worst) worst = loads[i].elapsed_us;
    }
    double ops = double(kThreads) * kIterations * 2 / (double(now_us() - t0) / 1e6);
    check(true, "malloc-priorities", "6 threads on one core, priorities 300/767: done in %.2f s, %.2f M malloc+free/s",
          worst / 1e6, ops / 1e6);
}

// --- malloc throughput, one thread per core ----------------------------------------------------
void* throughput_worker(void* arg) {
    auto* load = static_cast<MallocLoad*>(arg);
    uint64_t t0 = now_us();
    std::vector<void*> keep(256, nullptr);
    unsigned seed = static_cast<unsigned>(load->prio) * 7919u;
    for (int i = 0; i < load->iterations; i++) {
        seed = seed * 1103515245u + 12345u;
        int slot = (seed >> 8) & 255;
        free(keep[slot]);
        keep[slot] = malloc(16 + ((seed >> 16) & 1023));
    }
    for (void* p : keep) free(p);
    load->elapsed_us = now_us() - t0;
    return nullptr;
}

double malloc_rate(int threads) {
    constexpr int kIterations = 400000;
    std::atomic<int> done{0};
    std::vector<MallocLoad> loads(threads);
    std::vector<pthread_t> ids(threads);
    uint64_t t0 = now_us();
    for (int i = 0; i < threads; i++) {
        loads[i] = {i + 1, kIterations, &done, 0};
        pthread_create(&ids[i], nullptr, throughput_worker, &loads[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(ids[i], nullptr);
    double secs = double(now_us() - t0) / 1e6;
    return double(threads) * kIterations * 2 / secs / 1e6;
}

// Six threads on the one heap made a fifth of one thread's rate (v00.02: 1.06 vs 4.74 M/s). With
// automatic arenas, a thread that keeps waiting for the heap lock gets an arena of its own.
void test_malloc_throughput() {
    ps4_kit_set_auto_arena_waits(0);
    double one = malloc_rate(1);
    double shared = malloc_rate(6);
    ps4_kit_set_auto_arena_waits(32);
    int arenas0 = ps4_kit_auto_arena_count();
    double automatic = malloc_rate(6);
    report("malloc-throughput: 1 thread %.2f M/s; 6 threads on the shared heap %.2f M/s", one, shared);
    check(automatic > shared * 1.5, "malloc-auto-arenas",
          "6 threads with automatic arenas %.2f M/s (%.1fx the shared heap), %d new arena(s)", automatic,
          automatic / shared, ps4_kit_auto_arena_count() - arenas0);
}

// --- big allocations ---------------------------------------------------------------------------
// Spider-Man 2's save states needed one 99 MiB block with the main heap mostly full; the kit's
// overflow heap (direct memory) must serve such blocks.
void test_big_blocks() {
    std::vector<void*> blocks;
    size_t total = 0;
    for (int i = 0; i < 8; i++) {
        void* p = malloc(128u << 20);
        if (!p) break;
        memset(p, 0xA5, 4096);
        static_cast<char*>(p)[(128u << 20) - 1] = 1;
        blocks.push_back(p);
        total += 128u << 20;
    }
    for (void* p : blocks) free(p);
    check(total >= (512u << 20), "big-blocks", "%zu MiB in 128 MiB blocks", total >> 20);
}

// --- clock_gettime -----------------------------------------------------------------------------
// The kit answers CLOCK_MONOTONIC from the time stamp counter; a system call costs microseconds, and
// emulators read the clock thousands of times a frame.
void test_clock() {
    constexpr int kCalls = 200000;
    struct timespec ts, prev{};
    bool monotonic = true;
    uint64_t t0 = now_us();
    for (int i = 0; i < kCalls; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (ts.tv_sec < prev.tv_sec || (ts.tv_sec == prev.tv_sec && ts.tv_nsec < prev.tv_nsec)) monotonic = false;
        prev = ts;
    }
    double ns = double(now_us() - t0) * 1000.0 / kCalls;
    check(monotonic && ns < 500, "clock-monotonic", "%.0f ns per call%s", ns, monotonic ? "" : ", WENT BACKWARDS");
}

// --- new threads on every core -----------------------------------------------------------------
// The PS4 starts a thread on its creator's cores only; the kit's pthread_create wrap opens all of the
// app's cores to it.
std::atomic<uint32_t> g_cores_seen{0};
void* spin_worker(void*) {
    uint64_t t0 = now_us();
    volatile uint64_t x = 0;
    while (now_us() - t0 < 300'000) {
        x = x + 1;
        g_cores_seen.fetch_or(1u << sceKernelGetCurrentCpu());
    }
    return nullptr;
}

void test_thread_spread() {
    // Created from a thread pinned to one core.
    scePthreadSetaffinity(pthread_self(), 1u << 0);
    pthread_t ids[6];
    for (auto& id : ids) pthread_create(&id, nullptr, spin_worker, nullptr);
    for (auto& id : ids) pthread_join(id, nullptr);
    scePthreadSetaffinity(pthread_self(), 0x7f);
    uint32_t seen = g_cores_seen.load();
    int count = __builtin_popcount(seen);
    check(count >= 4, "thread-spread", "6 busy threads made by a thread on core 0 ran on %d cores (mask 0x%02x)",
          count, seen);
}

}  // namespace

void run_cpu_tests() {
    report("== CPU");
    test_clock();
    test_thread_spread();
    test_malloc_throughput();
    test_big_blocks();
    test_malloc_priorities();
}

}  // namespace kittest
