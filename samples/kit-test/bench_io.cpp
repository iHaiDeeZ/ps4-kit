// kit-test: files. std::filesystem goes through the *at functions, realpath and statvfs, which the
// OpenOrbis C library only has as ENOSYS stubs or broken (the kit's ps4_fileat.cpp fills them in).
#include "report.h"

#include <sys/statvfs.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace kittest {
namespace {

constexpr const char* kDir = "/data/ps4-kit/io-test";

void test_filesystem() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
    bool made = fs::create_directories(fs::path(kDir) / "a" / "b", ec);
    check(made && !ec, "fs-create-directories", "%s", ec ? ec.message().c_str() : "ok");

    {
        FILE* f = fopen((std::string(kDir) + "/a/b/x.txt").c_str(), "wb");
        if (f) { fputs("kit", f); fclose(f); }
    }
    fs::rename(fs::path(kDir) / "a/b/x.txt", fs::path(kDir) / "a/y.txt", ec);
    bool renamed = !ec && fs::exists(fs::path(kDir) / "a/y.txt");
    check(renamed, "fs-rename", "%s", ec ? ec.message().c_str() : "ok");

    int entries = 0;
    for (auto it = fs::recursive_directory_iterator(kDir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        entries++;
    check(!ec && entries == 3, "fs-iterate", "%d entries (expected 3)%s%s", entries, ec ? ": " : "",
          ec ? ec.message().c_str() : "");

    auto canon = fs::canonical(fs::path(kDir) / "a" / ".." / "a" / "y.txt", ec);
    check(!ec && canon == fs::path(kDir) / "a" / "y.txt", "fs-canonical", "%s",
          ec ? ec.message().c_str() : canon.c_str());

    auto space = fs::space(kDir, ec);
    check(!ec && space.capacity > 0 && space.available > 0, "fs-space", "%llu MiB free of %llu MiB",
          (unsigned long long)(space.available >> 20), (unsigned long long)(space.capacity >> 20));

    uintmax_t removed = fs::remove_all(kDir, ec);
    check(!ec && removed == 4, "fs-remove-all", "%llu removed%s%s", (unsigned long long)removed, ec ? ": " : "",
          ec ? ec.message().c_str() : "");
}

void test_throughput() {
    std::error_code ec;
    fs::create_directories(kDir, ec);
    std::string path = std::string(kDir) + "/big.bin";
    constexpr size_t kChunk = 1 << 20, kTotal = 128u << 20;
    std::vector<char> buf(kChunk, 'k');
    uint64_t t0 = now_us();
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { check(false, "io-write", "fopen failed"); return; }
    for (size_t done = 0; done < kTotal; done += kChunk) fwrite(buf.data(), 1, kChunk, f);
    fclose(f);
    double wsecs = double(now_us() - t0) / 1e6;
    t0 = now_us();
    f = fopen(path.c_str(), "rb");
    size_t got = 0, n;
    while (f && (n = fread(buf.data(), 1, kChunk, f)) > 0) got += n;
    if (f) fclose(f);
    double rsecs = double(now_us() - t0) / 1e6;
    check(got == kTotal, "io-throughput", "128 MiB: write %.0f MiB/s, read %.0f MiB/s (HDD, likely cached)",
          128 / wsecs, 128 / rsecs);
    fs::remove_all(kDir, ec);
}

// Threads printing to one unbuffered FILE at once. musl never locks its FILEs on the PS4 and points an
// unbuffered one at a buffer on the printing thread's stack, so without the kit's stdio lock
// (ps4_stdio.cpp) the lines come out garbled or the threads write each other's stacks.
void test_stdio_threads() {
    std::error_code ec;
    fs::create_directories(kDir, ec);
    std::string path = std::string(kDir) + "/stdio.txt";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { check(false, "stdio-threads", "fopen failed"); return; }
    setvbuf(f, nullptr, _IONBF, 0);
    constexpr int kThreads = 6, kLines = 2000;
    static const char kPayload[] = "the quick brown fox jumps over the lazy dog";
    uint64_t t0 = now_us();
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++)
        threads.emplace_back([f, t] {
            char text[128];
            for (int i = 0; i < kLines; i++) {  // one call per line: fprintf, fputs, fwrite
                if (i % 3 == 0) { fprintf(f, "t%d %05d %s\n", t, i, kPayload); continue; }
                int n = snprintf(text, sizeof text, "t%d %05d %s\n", t, i, kPayload);
                if (i % 3 == 1) fputs(text, f);
                else fwrite(text, 1, n, f);
            }
        });
    for (auto& th : threads) th.join();
    fclose(f);
    double ms = double(now_us() - t0) / 1e3;
    int good = 0, bad = 0;
    f = fopen(path.c_str(), "r");
    char line[256], want[256];
    while (f && fgets(line, sizeof line, f)) {
        int t = -1, i = -1;
        if (sscanf(line, "t%d %d", &t, &i) == 2)
            snprintf(want, sizeof want, "t%d %05d %s\n", t, i, kPayload);
        if (t >= 0 && i >= 0 && strcmp(line, want) == 0) good++;
        else bad++;
    }
    if (f) fclose(f);
    check(good == kThreads * kLines && bad == 0, "stdio-threads", "%d threads: %d of %d lines intact, %d garbled, %.0f ms",
          kThreads, good, kThreads * kLines, bad, ms);
    fs::remove_all(kDir, ec);
}

}  // namespace

void run_io_tests() {
    report("== Files");
    test_filesystem();
    test_throughput();
    test_stdio_threads();
}

}  // namespace kittest
