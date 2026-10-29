/* ps4-kit (from xemu-ps4): the PS4's struct dirent is FreeBSD 9's (d_fileno, d_reclen, d_type,
 * d_namlen, d_name), but OpenOrbis' dirent.h then defines d_fileno as d_ino, so neither name reaches
 * the member. FreeBSD's alias instead. It also announces a d_off the struct hasn't got
 * (_DIRENT_HAVE_D_OFF: code that trusts it doesn't compile) and not the d_namlen it has. */
#include_next <dirent.h>
#ifndef _PS4KIT_DIRENT_H
#define _PS4KIT_DIRENT_H
#undef d_fileno
#ifndef d_ino
#define d_ino d_fileno
#endif
#undef _DIRENT_HAVE_D_OFF
#ifndef _DIRENT_HAVE_D_NAMLEN
#define _DIRENT_HAVE_D_NAMLEN
#endif
#endif
