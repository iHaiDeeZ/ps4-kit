// kit-test: results go to /data/ps4-kit/kit-test.log (fetch it over FTP) and to the kernel log.
#pragma once

#include <cstdint>

namespace kittest {

// One line, printf-style, to the results file and the kernel log.
void report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// A named check: "PASS name: detail" / "FAIL name: detail". Counts failures for the summary.
void check(bool ok, const char* name, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int failures();

// Microseconds from the time stamp counter.
uint64_t now_us();

void run_cpu_tests();
void run_io_tests();
// Returns false when Vulkan could not start at all (reported).
bool run_gpu_tests();

}  // namespace kittest
