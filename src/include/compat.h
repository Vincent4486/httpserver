/* Small portability header to smooth differences between Windows (MSVC/MinGW) and POSIX */
#ifndef HTTP_COMPAT_H
#define HTTP_COMPAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
/* Use pointer-sized signed type for ssize_t on Windows */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

typedef intptr_t ssize_t;

#define PATH_SEPARATOR '\\'
#define PATH_SEPARATOR_STR "\\"

/* map common POSIX file I/O names to MSVCRT underscored names */
#define read _read
#define write _write
#define close _close
#define open _open
#define O_RDONLY _O_RDONLY
#define strcasecmp _stricmp

/* realpath -> _fullpath on Windows */
#ifndef realpath
#define realpath(N, R) _fullpath((R), (N), _MAX_PATH)
#endif

/* mkdir wrapper: map to _mkdir which ignores mode */
#ifndef mkdir
#define mkdir(p, mode) _mkdir(p)
#endif

/* Ensure PATH_MAX exists on Windows builds */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Provide POSIX-like S_ISDIR/S_ISREG on Windows if missing */
#ifndef S_ISDIR
#ifdef _S_IFDIR
#define S_ISDIR(m) (((m) & _S_IFDIR) == _S_IFDIR)
#else
#define S_ISDIR(m) 0
#endif
#endif
#ifndef S_ISREG
#ifdef _S_IFREG
#define S_ISREG(m) (((m) & _S_IFREG) == _S_IFREG)
#else
#define S_ISREG(m) 0
#endif
#endif

/* Ensure O_RDONLY is available under the POSIX name */
#ifndef O_RDONLY
#ifdef _O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#endif

/* socket helpers */
#define CLOSE_SOCKET(s) closesocket(s)
#define SOCKET_ERRNO() WSAGetLastError()

#else /* POSIX */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
/* sockets for POSIX */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PATH_SEPARATOR '/'
#define PATH_SEPARATOR_STR "/"

typedef off_t compat_off_t;

#define CLOSE_SOCKET(s) close(s)
#define SOCKET_ERRNO() errno

/* Ensure PATH_MAX is defined */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif /* _WIN32 */

#endif /* HTTP_COMPAT_H */
