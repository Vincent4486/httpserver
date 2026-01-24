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
#include <psapi.h>  /* for GetProcessMemoryInfo */
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>  /* for struct timeval */
#include <signal.h>     /* for signal() */

/* MinGW provides pthread.h through winpthreads */
#ifdef __MINGW32__
#include <pthread.h>
#endif

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

/* Don't define mkdir macro - let code use conditional compilation */

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

#ifndef __MINGW32__
/* pthread compatibility using Windows CRITICAL_SECTION (MSVC only) */
typedef CRITICAL_SECTION pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, void *attr) {
    (void)attr;
    InitializeCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}
#endif /* !__MINGW32__ */

/* gettimeofday implementation for Windows (both MinGW and MSVC) */
#ifndef __MINGW32__
static inline int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; /* timezone not supported */
    FILETIME ft;
    ULARGE_INTEGER uli;
    
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    
    /* Convert from 100-nanosecond intervals since 1601 to microseconds since 1970 */
    uint64_t t = uli.QuadPart / 10 - 11644473600000000ULL;
    tv->tv_sec = (long)(t / 1000000UL);
    tv->tv_usec = (long)(t % 1000000UL);
    return 0;
}
#endif /* !__MINGW32__ */

/* strptime implementation for Windows (both MinGW and MSVC) */
static inline char *strptime(const char *buf, const char *fmt, struct tm *tm) {
    (void)fmt; /* Only support the HTTP date format used in the code */
    
    /* Parse: "Wed, 21 Oct 2015 07:28:00 GMT" */
    const char *month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char wday[4], mon[4];
    int day, year, hour, min, sec;
    
    if (sscanf(buf, "%3s, %d %3s %d %d:%d:%d GMT", 
               wday, &day, mon, &year, &hour, &min, &sec) != 7) {
        return NULL;
    }
    
    tm->tm_mday = day;
    tm->tm_year = year - 1900;
    tm->tm_hour = hour;
    tm->tm_min = min;
    tm->tm_sec = sec;
    
    /* Find month */
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, month_names[i]) == 0) {
            tm->tm_mon = i;
            break;
        }
    }
    
    return (char *)buf + strlen(buf);
}

/* Signal handling stubs for Windows (signals work differently) */
#define SIGTERM 15
#define SIGINT 2

struct sigaction {
    void (*sa_handler)(int);
    int sa_flags;
    int sa_mask;
};

static inline int sigemptyset(int *set) {
    *set = 0;
    return 0;
}

static inline int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    (void)oldact;
    if (signum == SIGINT) {
        signal(SIGINT, act->sa_handler);
        return 0;
    }
    if (signum == SIGTERM) {
        signal(SIGTERM, act->sa_handler);
        return 0;
    }
    return -1;
}

/* Resource usage stubs for Windows */
#define RUSAGE_SELF 0

struct rusage {
    long ru_maxrss; /* maximum resident set size */
    struct timeval ru_utime; /* user CPU time */
    struct timeval ru_stime; /* system CPU time */
};

static inline int getrusage(int who, struct rusage *usage) {
    (void)who;
    /* Windows doesn't have getrusage - return minimal info */
    PROCESS_MEMORY_COUNTERS pmc;
    FILETIME createTime, exitTime, kernelTime, userTime;
    
    memset(usage, 0, sizeof(*usage));
    
    /* Get memory info */
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        usage->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024); /* Convert to KB */
    }
    
    /* Get CPU times */
    if (GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime)) {
        ULARGE_INTEGER uli;
        
        /* Convert user time */
        uli.LowPart = userTime.dwLowDateTime;
        uli.HighPart = userTime.dwHighDateTime;
        uint64_t user_us = uli.QuadPart / 10; /* Convert 100ns to microseconds */
        usage->ru_utime.tv_sec = (long)(user_us / 1000000UL);
        usage->ru_utime.tv_usec = (long)(user_us % 1000000UL);
        
        /* Convert system time */
        uli.LowPart = kernelTime.dwLowDateTime;
        uli.HighPart = kernelTime.dwHighDateTime;
        uint64_t sys_us = uli.QuadPart / 10; /* Convert 100ns to microseconds */
        usage->ru_stime.tv_sec = (long)(sys_us / 1000000UL);
        usage->ru_stime.tv_usec = (long)(sys_us % 1000000UL);
    }
    
    return 0;
}

#else /* POSIX */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
/* sockets for POSIX */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

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
