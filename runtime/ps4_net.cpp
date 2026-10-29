// Host name lookups on the PS4. The OpenOrbis C library's getaddrinfo and gethostbyname have no
// DNS configuration on the PS4, so names never resolve: online services (Pretendo, through curl
// and the games' own sockets) couldn't find their servers. With the linker's --wrap
// (ps4-kit.cmake) a name goes through libSceNet's resolver, and the address it finds goes back to
// the C library as text, so its results (and freeaddrinfo) stay its own.
// Numeric addresses and empty names go straight to the C library.
//
// Answers are kept for a minute and failures for 5 s: a lookup is a round trip to the router at
// best and 2 x 5 s when the name has no answer, and the share code (ps4_netfs.cpp) and games'
// reconnect loops ask for the same name again and again.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <chrono>
#include <mutex>
#include <string>

#include "ps4_kit_io.h"

extern "C" {
int __real_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);

// libSceNet (declared by hand: the OpenOrbis headers pull in their own socket types)
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
int sceNetResolverCreate(const char* name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char* hostname, uint32_t* address, int timeout_us, int retries,
                            int flags);
int sceNetResolverDestroy(int resolver);
}

// ps4_crashlog.cpp's boot trace. A do-nothing stand-in when it isn't linked: a weak reference left
// undefined would become an import, and create-fself refuses those ("missing module for symbol").
extern "C" __attribute__((weak)) void ps4_boot_trace(const char*) {}

namespace {

constexpr uint32_t kSysmoduleNet = 0x8000001C;  // ORBIS_SYSMODULE_INTERNAL_NET
constexpr auto kKeepFound = std::chrono::seconds(60);
constexpr auto kKeepMissing = std::chrono::seconds(5);
constexpr size_t kKept = 32;

void trace(const char* line) {
    ps4_boot_trace(line);
}

struct Kept {
    std::string name;
    uint32_t address = 0;  // network byte order; 0: no answer
    std::chrono::steady_clock::time_point until{};
};

std::mutex s_keptLock;
Kept s_kept[kKept];
size_t s_nextKept = 0;

bool findKept(const char* name, uint32_t& address) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(s_keptLock);
    for (const Kept& kept : s_kept) {
        if (now < kept.until && kept.name == name) {
            address = kept.address;
            return true;
        }
    }
    return false;
}

void keep(const char* name, uint32_t address) {
    const auto until = std::chrono::steady_clock::now() + (address ? kKeepFound : kKeepMissing);
    std::lock_guard lock(s_keptLock);
    Kept* slot = nullptr;
    for (Kept& kept : s_kept)
        if (kept.name == name)
            slot = &kept;
    if (!slot) {
        slot = &s_kept[s_nextKept];
        s_nextKept = (s_nextKept + 1) % kKept;
    }
    slot->name = name;
    slot->address = address;
    slot->until = until;
}

// The name's IPv4 address (network byte order) from libSceNet's resolver; false if it has none.
bool lookUp(const char* name, uint32_t& out) {
    // "localhost" isn't in any DNS, and musl would look for it in an /etc/hosts the PS4 hasn't got
    if (strcasecmp(name, "localhost") == 0) {
        out = htonl(INADDR_LOOPBACK);
        return true;
    }
    if (findKept(name, out))
        return out != 0;
    static std::once_flag s_init;
    static int s_pool = -1;
    std::call_once(s_init, [] {
        sceSysmoduleLoadModuleInternal(kSysmoduleNet);
        sceNetInit();
        s_pool = sceNetPoolCreate("ps4kit-dns", 64 * 1024, 0);
    });
    if (s_pool < 0)
        return false;
    const int resolver = sceNetResolverCreate("ps4kit-dns", s_pool, 0);
    if (resolver < 0)
        return false;  // (not kept: out of resolvers is no answer about the name)
    uint32_t address = 0;
    const int result = sceNetResolverStartNtoa(resolver, name, &address, 5 * 1000 * 1000, 2, 0);
    sceNetResolverDestroy(resolver);
    char line[200];
    if (result < 0 || address == 0) {
        snprintf(line, sizeof(line), "dns: %s not found (%#x)", name, static_cast<unsigned>(result));
        trace(line);
        keep(name, 0);
        return false;
    }
    snprintf(line, sizeof(line), "dns: %s is %u.%u.%u.%u", name, address & 0xFF, (address >> 8) & 0xFF,
             (address >> 16) & 0xFF, (address >> 24) & 0xFF);
    trace(line);
    keep(name, address);
    out = address;
    return true;
}

bool isNumeric(const char* name) {
    in_addr v4;
    in6_addr v6;
    return inet_pton(AF_INET, name, &v4) == 1 || inet_pton(AF_INET6, name, &v6) == 1;
}

}  // namespace

extern "C" int ps4_net_resolve(const char* name, char* text, size_t size) {
    uint32_t address = 0;
    if (!name || !*name || !lookUp(name, address))
        return 0;
    snprintf(text, size, "%u.%u.%u.%u", address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF,
             (address >> 24) & 0xFF);
    return 1;
}

extern "C" void ps4_net_forget(void) {
    std::lock_guard lock(s_keptLock);
    for (Kept& kept : s_kept)
        kept = Kept{};
}

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                                  struct addrinfo** res) {
    // M33: musl answers AI_ADDRCONFIG / AI_V4MAPPED by probing the network with Linux socket calls,
    // which the PS4's kernel refuses, so the lookup failed (PPSSPP asks for AI_ADDRCONFIG). The PS4
    // has IPv4: drop them.
    struct addrinfo plain = {};
    if (hints) {
        plain = *hints;
        plain.ai_flags &= ~(AI_ADDRCONFIG | AI_V4MAPPED);
        hints = &plain;
    }
    if (!node || !*node || isNumeric(node) || (hints && (hints->ai_flags & AI_NUMERICHOST)))
        return __real_getaddrinfo(node, service, hints, res);
    char address[64];
    if (!ps4_net_resolve(node, address, sizeof(address)))
        return EAI_NONAME;
    struct addrinfo numeric = {};
    if (hints)
        numeric = *hints;
    numeric.ai_flags |= AI_NUMERICHOST;
    if (numeric.ai_family == AF_UNSPEC)
        numeric.ai_family = AF_INET;
    return __real_getaddrinfo(address, service, &numeric, res);
}

// M33: gethostbyname itself (not wrapped), with a static IPv4 result of its own as the C library's
// is. The OpenOrbis libc.a's gethostbyname imports a function no console module has (create-fself:
// "missing module for symbol"), and with --wrap the linker kept a reference to it even unused.
namespace {
struct hostent* hostByName(const char* name) {
    static char s_name[256];
    static in_addr s_address;
    static char* s_addresses[2];
    static char* s_aliases[1];
    static hostent s_host;
    if (!name || !*name)
        return nullptr;
    if (inet_pton(AF_INET, name, &s_address) != 1) {
        uint32_t address = 0;
        if (!lookUp(name, address))
            return nullptr;
        s_address.s_addr = address;
    }
    snprintf(s_name, sizeof(s_name), "%s", name);
    s_addresses[0] = reinterpret_cast<char*>(&s_address);
    s_addresses[1] = nullptr;
    s_aliases[0] = nullptr;
    s_host.h_name = s_name;
    s_host.h_aliases = s_aliases;
    s_host.h_addrtype = AF_INET;
    s_host.h_length = sizeof(s_address);
    s_host.h_addr_list = s_addresses;
    return &s_host;
}
}  // namespace

extern "C" struct hostent* gethostbyname(const char* name) {
    return hostByName(name);
}

// Crema and the ports copied from it linked with --wrap=gethostbyname: their calls land here, on
// the same lookup (the C library's is never referenced). Not through the gethostbyname symbol: with
// that wrap in force the call would come back here.
extern "C" struct hostent* __wrap_gethostbyname(const char* name) {
    return hostByName(name);
}
