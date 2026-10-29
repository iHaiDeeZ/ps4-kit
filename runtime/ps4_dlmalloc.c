/* dlmalloc 2.8.6 (Doug Lea, MIT-0) configured as the PS4 app heap: only the mspace API (no
 * global malloc symbols, so nothing clashes with the OpenOrbis libc), over a memory block we
 * map ourselves (no mmap/sbrk), with locks and footers. Corruption and bad frees are reported
 * through ps4_heap_error() in ps4_runtime.cpp with the operation and caller that hit them.
 *
 * Replaces Sony's sceLibcMspace, which stopped returning memory after ~800 small allocations
 * in Dolphin's static initialisation with 49 KB in use on a 1 GiB heap. */

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
/* Blocking locks (pthread mutexes), not dlmalloc's x86 default spin lock: that spins and
 * sched_yield()s, and the PS4's scheduler only yields to threads of the same or higher priority.
 * A lower-priority thread holding the lock then never runs again while the higher-priority
 * threads spin for it: Crema 1.61 froze with every busy thread stuck in mspace_malloc behind a
 * recompiler worker, Conker v00.09 and v00.10 behind RT64's shader compilers (priority 767 on
 * cores 4-6), and TOTK the same way. */
#define USE_SPIN_LOCKS 0
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define FOOTERS 1
#define INSECURE 0
#define USE_DEV_RANDOM 0
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
/* The PS4 page size; sysconf numbering differs between headers, so don't ask. */
#define malloc_getpagesize ((size_t)16384U)
#define DEFAULT_GRANULARITY ((size_t)64U * (size_t)1024U)

/* Every heap lock is tried first; a thread that has to wait counts it (ps4_heap_lock_waits, per
 * thread, so counting costs nothing shared). ps4_runtime.cpp gives a thread that keeps waiting on
 * the main heap an arena of its own: six threads allocating at once on the one heap made 1.06 M
 * malloc+free per second together, a fifth of what one thread alone made (kit-test v00.02). The
 * uncontended path is the same single atomic operation as before (trylock instead of lock). */
#include <pthread.h>
__attribute__((tls_model("initial-exec"))) _Thread_local unsigned ps4_heap_lock_waits;
static inline int ps4_heap_lock(pthread_mutex_t* lk) {
  if (pthread_mutex_trylock(lk) == 0)
    return 0;
  ps4_heap_lock_waits++;
  return pthread_mutex_lock(lk);
}
#define pthread_mutex_lock(lk) ps4_heap_lock(lk)

void ps4_heap_error(void* mspace, void* chunk, int corruption);
#define CORRUPTION_ERROR_ACTION(m) ps4_heap_error((m), 0, 1)
#define USAGE_ERROR_ACTION(m, p) ps4_heap_error((m), (p), 0)

#include "third_party/dlmalloc/malloc.c"

/* dlmalloc's lock setup (pthread_init_lock) puts a pthread_mutexattr_t on the stack, and the
 * kernel's pthread_mutexattr_init stores an 8-byte pointer in it. musl's header type is 4 bytes:
 * built against it, every new mspace (the main heap, each arena and overflow heap) would overwrite
 * 4 bytes of its caller's stack. The FreeBSD-sized types come from the toolchain's libc-overlay
 * (bits/alltypes.h). */
_Static_assert(sizeof(pthread_mutexattr_t) >= 8,
               "pthread_mutexattr_t is musl's 4-byte type: build with the toolchain's libc-overlay");

/* Integrity check for diagnostics: is the mspace's state still intact? */
int ps4_heap_ok(void* msp) {
  mstate ms = (mstate)msp;
  return ok_magic(ms);
}

/* The free space above the highest chunk ever used (the top chunk): with the heap one block of
 * fixed size, capacity minus this is how far it has been filled (freed chunks below count). */
size_t ps4_heap_top_free(void* msp) {
  mstate ms = (mstate)msp;
  return ms->topsize;
}
