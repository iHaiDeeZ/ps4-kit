// kit-test: the kit's runtime, Mesa and presenter on the console, measured. Built with nothing but
// the kit (ps4_kit_app + ps4_kit_link_mesa), so it is also the template for a new port.
//
// Results: /data/ps4-kit/kit-test.log (and the kernel log). The screen shows the GPU test's frames,
// then green when every check passed or red when one failed, for 20 s; then the app exits.
#include "report.h"

#include <cstdlib>

#ifndef KIT_TEST_VERSION
#define KIT_TEST_VERSION "00.00"
#endif

int main() {
    using namespace kittest;
    report("ps4-kit test v" KIT_TEST_VERSION);
    run_cpu_tests();
    run_io_tests();
    run_gpu_tests();
    report("== %s: %d check(s) failed", failures() ? "FAILED" : "ALL PASSED", failures());
    return failures() ? 1 : 0;
}
