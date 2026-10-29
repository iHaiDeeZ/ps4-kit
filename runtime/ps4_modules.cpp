// The system modules the ports call into, loaded before main (SMW's platform_ps4.cpp
// load_system_modules; Conker, SSBU, HENkaku). Calling into a system library whose module isn't
// loaded ends the app without a word: Conker v00.00 and v00.01 died in their first sceVideoOutOpen
// that way (no signal, no crash.log).
//
// The kit loads what every port with a display and a pad needs; an app adds its own with
// ps4_kit_extra_modules (ps4_kit_io.h), or loads one later with ps4_kit_load_module.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ps4_kit_io.h"

extern "C" int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
// ps4_crashlog.cpp's boot trace. A do-nothing stand-in when it isn't linked: a weak reference left
// undefined would become an import, and create-fself refuses those ("missing module for symbol").
extern "C" __attribute__((weak)) void ps4_boot_trace(const char*) {}

// The app's list, if it defines one (ps4_kit_io.h); this empty one otherwise (also not left an
// undefined weak reference, for the same reason).
extern "C" __attribute__((weak)) const uint32_t* ps4_kit_extra_modules(size_t* count) {
    *count = 0;
    return nullptr;
}

extern "C" int ps4_kit_load_module(uint32_t id, const char* name) {
    const int r = sceSysmoduleLoadModuleInternal(id);
    if (r != 0) {
        char line[96];
        snprintf(line, sizeof line, "loading system module %s (%#x) failed (%x)", name ? name : "?", unsigned(id),
                 unsigned(r));
        ps4_boot_trace(line);
    }
    return r;
}

namespace {

// 102: after the C library's own setup, ahead of the app's constructors (which may open the
// display or the pad already)
__attribute__((constructor(102))) void load_system_modules() {
    const struct {
        const char* name;
        uint32_t id;
    } modules[] = {
        {"SystemService", 0x80000010}, {"UserService", 0x80000011}, {"VideoOut", 0x80000022},
        {"AudioOut", 0x80000001},      {"Pad", 0x80000024},
    };
    for (const auto& m : modules)
        ps4_kit_load_module(m.id, m.name);
    size_t count = 0;
    const uint32_t* ids = ps4_kit_extra_modules(&count);
    for (size_t i = 0; ids && i < count; i++)
        ps4_kit_load_module(ids[i], "(app's)");
    ps4_boot_trace("system modules loaded");
}

}  // namespace
