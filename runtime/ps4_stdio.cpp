// One lock around stdio output (linker --wrap, ps4-kit.cmake). The PS4's C library is OpenOrbis'
// musl, linked in statically; musl turns its per-FILE locks on in its own pthread_create, which never
// runs here (threads come from libkernel), so stdout and stderr are never locked. Printing to an
// unbuffered stream, musl points the FILE at a buffer on the printing thread's stack: two threads
// logging at once wrote each other's stacks (TTT2 v00.06's garbled log lines; v00.07's crash with a
// saved register holding " 0000000"). Every output call of the program - the runtime's, Mesa's,
// libc++'s, the app's - goes through these, one at a time. Recursive: a call may print from inside
// another (musl's printf calls vfprintf, a signal handler may print).
//
// The wrapped names are also linked with --undefined: a wrap whose function nothing else refers to
// leaves __real_<name> unresolved, an import create-fself refuses ("missing module for symbol"; TTT2
// with vprintf, putc, putchar and perror). Pulling the libc members in costs a few hundred bytes.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include <pthread.h>

namespace {
pthread_mutex_t g_stdio;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

void init() {
    // 8 bytes even if the headers' pthread_mutexattr_t is the 4-byte musl one (libkernel's is a pointer).
    void* attr[2] = {nullptr, nullptr};
    pthread_mutexattr_t* a = reinterpret_cast<pthread_mutexattr_t*>(attr);
    pthread_mutexattr_init(a);
    pthread_mutexattr_settype(a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_stdio, a);
    pthread_mutexattr_destroy(a);
}

struct Hold {
    Hold() {
        pthread_once(&g_once, init);
        pthread_mutex_lock(&g_stdio);
    }
    ~Hold() { pthread_mutex_unlock(&g_stdio); }
};
} // namespace

extern "C" {
int __real_vfprintf(FILE* f, const char* fmt, va_list ap);
int __real_vprintf(const char* fmt, va_list ap);
int __real_fputs(const char* s, FILE* f);
int __real_puts(const char* s);
int __real_fputc(int c, FILE* f);
int __real_putc(int c, FILE* f);
int __real_putchar(int c);
size_t __real_fwrite(const void* p, size_t size, size_t n, FILE* f);
int __real_fflush(FILE* f);
void __real_perror(const char* s);

int __wrap_vfprintf(FILE* f, const char* fmt, va_list ap) {
    Hold h;
    return __real_vfprintf(f, fmt, ap);
}
int __wrap_vprintf(const char* fmt, va_list ap) {
    Hold h;
    return __real_vprintf(fmt, ap);
}
int __wrap_fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r;
    {
        Hold h;
        r = __real_vfprintf(f, fmt, ap);
    }
    va_end(ap);
    return r;
}
int __wrap_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r;
    {
        Hold h;
        r = __real_vfprintf(stdout, fmt, ap);
    }
    va_end(ap);
    return r;
}
// clang turns printf("text\n") into puts, printf("x") into putchar, fprintf(f, "text") into fwrite.
int __wrap_fputs(const char* s, FILE* f) {
    Hold h;
    return __real_fputs(s, f);
}
int __wrap_puts(const char* s) {
    Hold h;
    return __real_puts(s);
}
int __wrap_fputc(int c, FILE* f) {
    Hold h;
    return __real_fputc(c, f);
}
int __wrap_putc(int c, FILE* f) {
    Hold h;
    return __real_putc(c, f);
}
int __wrap_putchar(int c) {
    Hold h;
    return __real_putchar(c);
}
size_t __wrap_fwrite(const void* p, size_t size, size_t n, FILE* f) {
    Hold h;
    return __real_fwrite(p, size, n, f);
}
int __wrap_fflush(FILE* f) {
    Hold h;
    return __real_fflush(f);
}
void __wrap_perror(const char* s) {
    Hold h;
    __real_perror(s);
}
}
