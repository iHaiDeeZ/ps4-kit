// FreeBSD C library functions the PS4's C library (OpenOrbis musl + libkernel) lacks, for code that
// takes its FreeBSD paths on the PS4 (from rpcs3; declared in the libc overlay's sys/mman.h by
// toolchain/prepare-sysroot.sh).
#include <stddef.h>

// getpagesizes(3): the page sizes the kernel maps. The PS4 maps memory in 16 KiB pages and can use
// 2 MiB large pages (direct memory). asmjit's large page detection asks for them (rpcs3).
int getpagesizes(size_t *pagesizes, int count)
{
    static const size_t sizes[] = {16384, 2 * 1024 * 1024};
    const int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
    if (pagesizes == NULL)
        return n;
    int i = 0;
    for (; i < count && i < n; i++)
        pagesizes[i] = sizes[i];
    return i;
}
