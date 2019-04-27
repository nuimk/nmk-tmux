/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef COMPAT_H
#define COMPAT_H

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

/*
 * Shouldn't be needed, but GNU/kFreeBSD headers are currently slightly broken.
 * The glibc limits.h eventually includes the FreeBSD limits-related headers,
 * which don't define a TTY_NAME_MAX. However, anything (in)directly including
 * the glibc sys/param.h will include the glibc bits/param.h, which defines
 * TTY_NAME_MAX as SPECNAMELEN, i.e. 63, which differs from our fallback of 32.
 * Thus, without this hack, different source files can (and do) end up with
 * different values for TTY_NAME_MAX, which among other things affects the
 * layout of struct window_pane due to the tty buffer.
 */
#ifdef __FreeBSD_kernel__
#include <sys/param.h>
#endif

#include <limits.h>
#include <stdio.h>
#include <termios.h>
#include <wchar.h>

#ifndef __GNUC__
#define __attribute__(a)
#endif

#ifndef __unused
#define __unused __attribute__ ((__unused__))
#endif
#ifndef __dead
#define __dead __attribute__ ((__noreturn__))
#endif
#ifndef __packed
#define __packed __attribute__ ((__packed__))
#endif

#ifndef ECHOPRT
#define ECHOPRT 0
#endif

#ifndef ACCESSPERMS
#define ACCESSPERMS (S_IRWXU|S_IRWXG|S_IRWXO)
#endif

#if !defined(FIONREAD) && defined(__sun)
#include <sys/filio.h>
#endif

#ifdef HAVE_ERR_H
#include <err.h>
#else
void	err(int, const char *, ...);
void	errx(int, const char *, ...);
void	warn(const char *, ...);
void	warnx(const char *, ...);
#endif

#ifndef HAVE_PATHS_H
#define	_PATH_BSHELL	"/bin/sh"
#define	_PATH_TMP	"/tmp/"
#define _PATH_DEVNULL	"/dev/null"
#define _PATH_TTY	"/dev/tty"
#define _PATH_DEV	"/dev/"
#endif

#ifndef __OpenBSD__
#define pledge(s, p) (0)
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#include <inttypes.h>
#endif

#ifdef HAVE_QUEUE_H
#include <sys/queue.h>
#else
#include "compat/queue.h"
#endif

#ifdef HAVE_TREE_H
#include <sys/tree.h>
#else
#include "compat/tree.h"
#endif

#ifdef HAVE_BITSTRING_H
#include <bitstring.h>
#else
#include "compat/bitstring.h"
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif

#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#ifdef HAVE_PTY_H
#include <pty.h>
#endif

#ifdef HAVE_UTIL_H
#include <util.h>
#endif

#ifdef HAVE_VIS
#include <vis.h>
#else
#include "compat/vis.h"
#endif

#ifdef HAVE_IMSG
#include <imsg.h>
#else
#include "compat/imsg.h"
#endif

#ifdef BROKEN_CMSG_FIRSTHDR
#undef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR(mhdr) \
	((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? \
	    (struct cmsghdr *)(mhdr)->msg_control :	    \
	    (struct cmsghdr *)NULL)
#endif

#ifndef CMSG_ALIGN
#ifdef _CMSG_DATA_ALIGN
#define CMSG_ALIGN _CMSG_DATA_ALIGN
#else
#define CMSG_ALIGN(len) (((len) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#endif
#endif

#ifndef CMSG_SPACE
#define CMSG_SPACE(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#endif

#ifndef CMSG_LEN
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#ifndef INFTIM
#define INFTIM -1
#endif

#ifndef WAIT_ANY
#define WAIT_ANY -1
#endif

#ifndef SUN_LEN
#define SUN_LEN(sun) (sizeof (sun)->sun_path)
#endif

#ifndef timercmp
#define	timercmp(tvp, uvp, cmp)						\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?				\
	    ((tvp)->tv_usec cmp (uvp)->tv_usec) :			\
	    ((tvp)->tv_sec cmp (uvp)->tv_sec))
#endif

#ifndef timeradd
#define	timeradd(tvp, uvp, vvp)						\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;		\
		(vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;	\
		if ((vvp)->tv_usec >= 1000000) {			\
			(vvp)->tv_sec++;				\
			(vvp)->tv_usec -= 1000000;			\
		}							\
	} while (0)
#endif

#ifndef timersub
#define timersub(tvp, uvp, vvp)                                         \
	do {                                                            \
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;          \
		(vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;       \
		if ((vvp)->tv_usec < 0) {                               \
			(vvp)->tv_sec--;                                \
			(vvp)->tv_usec += 1000000;                      \
		}                                                       \
	} while (0)
#endif

#ifndef TTY_NAME_MAX
#define TTY_NAME_MAX 32
#endif

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#ifndef HAVE_FLOCK
#define LOCK_SH 0
#define LOCK_EX 0
#define LOCK_NB 0
#define flock(fd, op) (0)
#endif

#ifndef HAVE_EXPLICIT_BZERO
/* explicit_bzero.c */
void		 explicit_bzero(void *, size_t);
#endif

#ifndef HAVE_GETDTABLECOUNT
/* getdtablecount.c */
int		 getdtablecount(void);
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif

#ifndef IOV_MAX
#define IOV_MAX	1024
#endif

#ifndef HAVE_CLOSEFROM
/* closefrom.c */
void		 closefrom(int);
#endif

#ifndef HAVE_STRCASESTR
/* strcasestr.c */
char		*strcasestr(const char *, const char *);
#endif

#ifndef HAVE_STRSEP
/* strsep.c */
char		*strsep(char **, const char *);
#endif

#ifndef HAVE_STRTONUM
/* strtonum.c */
long long	 strtonum(const char *, long long, long long, const char **);
#endif

#ifndef HAVE_STRLCPY
/* strlcpy.c */
size_t	 	 strlcpy(char *, const char *, size_t);
#endif

#ifndef HAVE_STRLCAT
/* strlcat.c */
size_t	 	 strlcat(char *, const char *, size_t);
#endif

#ifndef HAVE_STRNLEN
/* strnlen.c */
size_t		 strnlen(const char *, size_t);
#endif

#ifndef HAVE_STRNDUP
/* strndup.c */
char		*strndup(const char *, size_t);
#endif

#ifndef HAVE_MEMMEM
/* memmem.c */
void		*memmem(const void *, size_t, const void *, size_t);
#endif

#ifndef HAVE_DAEMON
/* daemon.c */
int	 	 daemon(int, int);
#endif

#ifndef HAVE_GETPROGNAME
/* getprogname.c */
const char	*getprogname(void);
#endif

#ifndef HAVE_SETPROCTITLE
/* setproctitle.c */
void		 setproctitle(const char *, ...);
#endif

#ifndef HAVE_B64_NTOP
/* base64.c */
#undef b64_ntop
#undef b64_pton
int		 b64_ntop(const char *, size_t, char *, size_t);
int		 b64_pton(const char *, u_char *, size_t);
#endif

#ifndef HAVE_FDFORKPTY
/* fdforkpty.c */
int		 getptmfd(void);
pid_t		 fdforkpty(int, int *, char *, struct termios *,
		     struct winsize *);
#endif

#ifndef HAVE_FORKPTY
/* forkpty.c */
pid_t		 forkpty(int *, char *, struct termios *, struct winsize *);
#endif

#ifndef HAVE_ASPRINTF
/* asprintf.c */
int		 asprintf(char **, const char *, ...);
int		 vasprintf(char **, const char *, va_list);
#endif

#ifndef HAVE_FGETLN
/* fgetln.c */
char		*fgetln(FILE *, size_t *);
#endif

#ifndef HAVE_FPARSELN
char		*fparseln(FILE *, size_t *, size_t *, const char *, int);
#endif

#ifndef HAVE_SETENV
/* setenv.c */
int		 setenv(const char *, const char *, int);
int		 unsetenv(const char *);
#endif

#ifndef HAVE_CFMAKERAW
/* cfmakeraw.c */
void		 cfmakeraw(struct termios *);
#endif

#ifndef HAVE_FREEZERO
/* freezero.c */
void		 freezero(void *, size_t);
#endif

#ifndef HAVE_REALLOCARRAY
/* reallocarray.c */
void		*reallocarray(void *, size_t, size_t);
#endif

#ifndef HAVE_RECALLOCARRAY
/* recallocarray.c */
void		*recallocarray(void *, size_t, size_t, size_t);
#endif

#ifdef HAVE_UTF8PROC
/* utf8proc.c */
int		 utf8proc_wcwidth(wchar_t);
int		 utf8proc_mbtowc(wchar_t *, const char *, size_t);
int		 utf8proc_wctomb(char *, wchar_t);
#endif

#ifndef HAVE_GETOPT
/* getopt.c */
extern int	BSDopterr;
extern int	BSDoptind;
extern int	BSDoptopt;
extern int	BSDoptreset;
extern char    *BSDoptarg;
int	BSDgetopt(int, char *const *, const char *);
#define getopt(ac, av, o)  BSDgetopt(ac, av, o)
#define opterr             BSDopterr
#define optind             BSDoptind
#define optopt             BSDoptopt
#define optreset           BSDoptreset
#define optarg             BSDoptarg
#endif

#endif /* COMPAT_H */
