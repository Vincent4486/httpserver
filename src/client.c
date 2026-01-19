#include <stdio.h>    // printf, perror
#include <stdlib.h>   // exit, EXIT_FAILURE
#include <string.h>   // strlen, strcpy, memset
#include <stdbool.h>  // bool, true, false
#include <stdint.h>   // intmax_t
#include <inttypes.h> // PRIdMAX
#include <ctype.h>    // isxdigit
#include <time.h>     // time, strptime, mktime

#include "include/compat.h"
#include <limits.h> // PATH_MAX

#include "include/client.h"
#include "include/logger.h"
#include "include/http.h"
#include "include/whitelist.h"
#include "include/settings.h"
#include "include/metrics.h"
#include "include/shutdown.h"
#include "include/access_log.h"
#include "include/threadpool.h"

static cache_entry_t cache[CACHE_MAX_ENTRIES];
static int cache_count = 0;

/* Helper function to handle HTTP request with timing */
static void handle_http_request_with_timing(int client_fd, const char *client_ip, const char *content_directory, bool show_ext)
{
    struct timeval start, end;
    gettimeofday(&start, NULL);

    handle_http_request(client_fd, client_ip, content_directory, show_ext);

    gettimeofday(&end, NULL);
    long seconds = end.tv_sec - start.tv_sec;
    long microseconds = end.tv_usec - start.tv_usec;
    double elapsed = seconds + microseconds * 1e-6;
    double elapsed_ms = elapsed * 1000.0;

    /* Record metrics (estimate 1KB per request as baseline) */
    metrics_record_request(1024, elapsed_ms);

    char timing_msg[128];
    snprintf(timing_msg, sizeof(timing_msg), "Request handled in %.3f ms", elapsed_ms);
    log_info(timing_msg);
}

/* Initialize cache */
void cache_init(void)
{
    memset(cache, 0, sizeof(cache));
}

/* Get cached file data */
cache_entry_t *cache_get(const char *path)
{
    for (int i = 0; i < cache_count; i++)
    {
        if (strcmp(cache[i].path, path) == 0)
        {
            struct stat st;
            if (stat(path, &st) == 0 && st.st_mtime == cache[i].mtime)
            {
                cache[i].cached_at = time(NULL); // Update access time
                return &cache[i];
            }
            else
            {
                // File changed, remove from cache
                free(cache[i].data);
                memmove(&cache[i], &cache[i + 1], (cache_count - i - 1) * sizeof(cache_entry_t));
                cache_count--;
                i--; // Recheck this position
            }
        }
    }
    return NULL;
}

/* Add file to cache (LRU eviction) */
void cache_put(const char *path, const char *data, size_t size, const char *mime_type, time_t mtime)
{
    // Don't cache if too big or already cached
    if (size > CACHE_MAX_FILE_SIZE || cache_get(path) != NULL)
        return;

    // Find LRU entry or add new one
    int replace_idx = -1;
    time_t oldest = time(NULL);

    for (int i = 0; i < cache_count; i++)
    {
        if (cache[i].cached_at < oldest)
        {
            oldest = cache[i].cached_at;
            replace_idx = i;
        }
    }

    if (cache_count < CACHE_MAX_ENTRIES)
    {
        replace_idx = cache_count++;
    }

    if (replace_idx >= 0)
    {
        free(cache[replace_idx].data);
        strncpy(cache[replace_idx].path, path, sizeof(cache[replace_idx].path) - 1);
        cache[replace_idx].data = malloc(size);
        if (cache[replace_idx].data)
        {
            memcpy(cache[replace_idx].data, data, size);
            cache[replace_idx].size = size;
            cache[replace_idx].mime_type = mime_type;
            cache[replace_idx].mtime = mtime;
            cache[replace_idx].cached_at = time(NULL);
        }
    }
}

int url_decode(char *s)
{
    char *dst = s;
    while (*s)
    {
        if (*s == '%')
        {
            if (!isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2]))
                return -1;
            char hex[3] = {s[1], s[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            s += 3;
        }
        else if (*s == '+')
        {
            *dst++ = ' ';
            s++;
        }
        else
        {
            *dst++ = *s++;
        }
    }
    *dst = '\0';
    return 0;
}

/* Parse HTTP date format: "Wed, 21 Oct 2015 07:28:00 GMT" */
static time_t parse_http_date(const char *date_str)
{
    struct tm tm = {0};
    char *result = strptime(date_str, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    if (!result)
        return -1;
    return mktime(&tm);
}

/* Extract If-Modified-Since header value from request buffer */
int get_if_modified_since(const char *request_buf, time_t *out_time)
{
    const char *header = strstr(request_buf, "If-Modified-Since:");
    if (!header)
        return 0;

    header += strlen("If-Modified-Since:");
    while (*header == ' ')
        header++;

    char date_str[100];
    const char *end = strchr(header, '\r');
    if (!end)
        end = strchr(header, '\n');
    if (!end)
        return 0;

    size_t len = end - header;
    if (len >= sizeof(date_str))
        return 0;

    strncpy(date_str, header, len);
    date_str[len] = '\0';

    *out_time = parse_http_date(date_str);
    return (*out_time > 0) ? 1 : 0;
}

/* Parse Range header: "bytes=0-100" or "bytes=100-" */
int parse_range_header(const char *request_buf, off_t file_size, off_t *out_start, off_t *out_end)
{
    const char *range = strstr(request_buf, "Range:");
    if (!range)
        return 0;

    range += strlen("Range:");
    while (*range == ' ')
        range++;

    if (strncmp(range, "bytes=", 6) != 0)
        return 0;

    range += 6;

    char *end_ptr;
    off_t start = strtoll(range, &end_ptr, 10);

    if (*end_ptr != '-')
        return 0;

    off_t end;
    if (*(end_ptr + 1) == '\r' || *(end_ptr + 1) == '\n')
    {
        /* "bytes=100-" means from 100 to end of file */
        end = file_size - 1;
    }
    else
    {
        end = strtoll(end_ptr + 1, NULL, 10);
    }

    /* Validate range */
    if (start < 0 || start >= file_size || end < start || end >= file_size)
        return 0;

    *out_start = start;
    *out_end = end;
    return 1;
}
void send_404(int client_fd)
{
    const char *not_found =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "404 Not Found";
    write(client_fd, not_found, strlen(not_found));
}

void send_403(int client_fd)
{
    const char *forbidden =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "Forbidden";
    write(client_fd, forbidden, strlen(forbidden));
}

void send_304(int client_fd)
{
    const char *not_modified =
        "HTTP/1.1 304 Not Modified\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    write(client_fd, not_modified, strlen(not_modified));
}

void send_206_header(int client_fd, const char *mime, off_t range_start, off_t range_end, off_t total_size)
{
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 206 Partial Content\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Range: bytes %jd-%jd/%jd\r\n"
                              "Content-Length: %jd\r\n"
                              "Accept-Ranges: bytes\r\n"
                              "\r\n",
                              mime, (intmax_t)range_start, (intmax_t)range_end,
                              (intmax_t)total_size, (intmax_t)(range_end - range_start + 1));
    if (header_len > 0)
        write(client_fd, header, header_len);
}

void send_301_location(int client_fd, const char *location)
{
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 301 Moved Permanently\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n",
                     location);
    if (n > 0)
        write(client_fd, hdr, n);
}

/* small helper to write a 200 header */
void send_200_header(int client_fd, const char *mime, off_t len)
{
    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %jd\r\n"
                              "\r\n",
                              mime, (intmax_t)len);
    if (header_len > 0)
        write(client_fd, header, header_len);
}

/* small helper to write a 200 header with keep-alive */
void send_200_header_keepalive(int client_fd, const char *mime, off_t len)
{
    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %jd\r\n"
                              "Connection: keep-alive\r\n"
                              "\r\n",
                              mime, (intmax_t)len);
    if (header_len > 0)
        write(client_fd, header, header_len);
}

const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    /* Use faster hash-like approach based on extension length and first char */
    size_t ext_len = strlen(ext);
    char first = ext[0];
    char second = ext_len > 1 ? ext[1] : '\0';

    /* Quick pre-filter by length and common patterns */
    if (ext_len == 5) /* .html, .jpeg */
    {
        if (strcmp(ext, ".html") == 0)
            return "text/html";
        if (strcmp(ext, ".jpeg") == 0)
            return "image/jpeg";
    }
    else if (ext_len == 4)
    {
        if (strcmp(ext, ".css") == 0)
            return "text/css";
        if (strcmp(ext, ".json") == 0)
            return "application/json";
        if (strcmp(ext, ".html") == 0)
            return "text/html";
        if (strcmp(ext, ".jpeg") == 0)
            return "image/jpeg";
    }
    else if (ext_len == 3)
    {
        if (strcmp(ext, ".js") == 0)
            return "application/javascript";
        if (strcmp(ext, ".png") == 0)
            return "image/png";
        if (strcmp(ext, ".jpg") == 0)
            return "image/jpeg";
        if (strcmp(ext, ".gif") == 0)
            return "image/gif";
        if (strcmp(ext, ".svg") == 0)
            return "image/svg+xml";
        if (strcmp(ext, ".ico") == 0)
            return "image/x-icon";
        if (strcmp(ext, ".xml") == 0)
            return "application/xml";
        if (strcmp(ext, ".pdf") == 0)
            return "application/pdf";
    }

    return "application/octet-stream";
}

/* Write buffer to socket, handling partial writes */
int write_buffer_fully(int client_fd, const char *buf, ssize_t size)
{
    const char *p = buf;
    while (size > 0)
    {
        ssize_t wn = write(client_fd, p, (unsigned int)size);
        if (wn <= 0)
            return -1;
        size -= wn;
        p += wn;
    }
    return 0;
}

/* Stream file using optimized buffer size for better performance */
int stream_file_fd(int client_fd, int fd, off_t filesize)
{
    static char buf[131072]; /* Increased to 128KB for better performance */
    ssize_t r;
    off_t remaining = filesize;

    while (remaining > 0)
    {
        size_t toread = remaining > (off_t)sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        r = read(fd, buf, toread);
        if (r <= 0)
            return -1;
        if (write_buffer_fully(client_fd, buf, r) != 0)
            return -1;
        remaining -= (off_t)r;
    }
    return 0;
}

#define TYPE_HTML 0
#define TYPE_PHP 1
#define TYPE_PERL 2
#define TYPE_UNKNOWN 3

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Check file extension and return type */
static int get_type_by_extension(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return TYPE_UNKNOWN;

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0)
        return TYPE_HTML;
    if (strcasecmp(ext, ".php") == 0)
        return TYPE_PHP;
    if (strcasecmp(ext, ".pl") == 0)
        return TYPE_PERL;

    return TYPE_UNKNOWN;
}

/* Check for index files in directory */
static int find_index_in_dir(const char *dir)
{
    char test[PATH_MAX];

    snprintf(test, sizeof(test), "%s%sindex.html", dir, PATH_SEPARATOR_STR);
    if (file_exists(test))
        return TYPE_HTML;

    snprintf(test, sizeof(test), "%s%sindex.php", dir, PATH_SEPARATOR_STR);
    if (file_exists(test))
        return TYPE_PHP;

    snprintf(test, sizeof(test), "%s%sindex.pl", dir, PATH_SEPARATOR_STR);
    if (file_exists(test))
        return TYPE_PERL;

    return TYPE_UNKNOWN;
}

/* Try appending extensions to base path */
static int try_with_extensions(const char *base)
{
    char test[PATH_MAX];

    snprintf(test, sizeof(test), "%s.html", base);
    if (file_exists(test))
        return TYPE_HTML;

    snprintf(test, sizeof(test), "%s.php", base);
    if (file_exists(test))
        return TYPE_PHP;

    snprintf(test, sizeof(test), "%s.pl", base);
    if (file_exists(test))
        return TYPE_PERL;

    snprintf(test, sizeof(test), "%s/index.html", base);
    if (file_exists(test))
        return TYPE_HTML;

    snprintf(test, sizeof(test), "%s/index.php", base);
    if (file_exists(test))
        return TYPE_PHP;

    snprintf(test, sizeof(test), "%s/index.pl", base);
    if (file_exists(test))
        return TYPE_PERL;

    return TYPE_UNKNOWN;
}

/* Normalize path by removing trailing slashes */
static void normalize_path(char *path)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
    {
        path[len - 1] = '\0';
        --len;
    }
}

int determine_file_type(const char *resolved_path)
{
    char path[PATH_MAX];
    struct stat st;

    if (!resolved_path || strlen(resolved_path) >= sizeof(path))
        return TYPE_UNKNOWN;

    strcpy(path, resolved_path);
    normalize_path(path);

    if (stat(path, &st) != 0)
        return try_with_extensions(path);

    if (S_ISDIR(st.st_mode))
        return find_index_in_dir(path);

    if (S_ISREG(st.st_mode))
        return get_type_by_extension(path);

    return TYPE_UNKNOWN;
}

/* join directory and request path safely into out (must be PATH_MAX). returns 0 on success */
int join_path(const char *dir, const char *req, char *out, size_t outlen)
{
    if (!dir || !req || !out)
        return -1;
    size_t dlen = strlen(dir);
    if (dlen >= outlen)
        return -1;
    if (dir[dlen - 1] == '/')
    {
        if (snprintf(out, outlen, "%s%s", dir, req[0] == '/' ? req + 1 : req) >= (int)outlen)
            return -1;
    }
    else
    {
        if (snprintf(out, outlen, "%s%s", dir, req) >= (int)outlen)
            return -1;
    }
    return 0;
}

/* Handle a single accepted client connection with keep-alive support */
void handle_accepted_client(int client_fd, struct sockaddr_in client_addr,
                            const char *content_directory, const bool show_ext)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(client_addr.sin_port);

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Accepted connection from %s:%d", client_ip, client_port);
    log_info(log_msg);

    /* Check whitelist if enabled */
    if (get_whitelist_enabled())
    {
        int whitelist_count = 0;
        char **whitelist_entries = get_whitelist_ips(&whitelist_count);

        if (whitelist_count > 0 && !is_ip_whitelisted(client_ip, whitelist_entries, whitelist_count))
        {
            char blocked_msg[128];
            snprintf(blocked_msg, sizeof(blocked_msg), "Connection from %s blocked by whitelist", client_ip);
            log_info(blocked_msg);
            send_403(client_fd);
            free_whitelist_entries(whitelist_entries, whitelist_count);
            close(client_fd);
            return;
        }

        if (whitelist_entries)
            free_whitelist_entries(whitelist_entries, whitelist_count);
    }

#ifdef _WIN32
    /* Windows MSYS: Simplified keep-alive - just handle one request per connection */
    /* Windows socket timeout handling is complex in MSYS, so keep it simple */
    handle_http_request_with_timing(client_fd, client_ip, content_directory, show_ext);
#else
    /* POSIX: Full keep-alive support with multiple requests per connection */
    time_t start_time = time(NULL);
    int request_count = 0;

    while (1)
    {
        /* Check for keep-alive timeout (30 seconds) */
        if (request_count > 0 && time(NULL) - start_time > 30)
            break;

        /* Set socket timeout for reading (5 seconds) */
        struct timeval tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* Try to read request */
        char peek_buf[1];
        ssize_t peek_result = recv(client_fd, peek_buf, 1, MSG_PEEK);
        if (peek_result <= 0)
            break; /* No more data or connection closed */

        /* Reset timeout for normal operation */
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_http_request_with_timing(client_fd, client_ip, content_directory, show_ext);
        request_count++;

        /* Limit requests per connection to prevent abuse */
        if (request_count >= 100)
            break;
    }

    if (request_count > 1)
    {
        char perf_msg[128];
        snprintf(perf_msg, sizeof(perf_msg), "Connection served %d requests", request_count);
        log_info(perf_msg);
    }
#endif

    close(client_fd);
}

void run_server_loop(int server_fd, const char *content_directory, const bool show_ext)
{
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        log_error_code(12, "Failed to initialize Winsock");
        return;
    }
#endif

    /* Main server loop with graceful shutdown support */
    while (!is_shutdown_requested())
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0)
        {
            /* EINTR means accept was interrupted by a signal - check if shutdown was requested */
            if (errno == EINTR)
            {
                if (is_shutdown_requested())
                    break;
                continue;
            }

            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "accept() failed: %s", strerror(errno));
            log_error_code(15, "%s", err_msg);
            continue;
        }

        handle_accepted_client(client_fd, client_addr, content_directory, show_ext);
    }

    log_info("Graceful shutdown initiated");

#ifdef _WIN32
    WSACleanup();
#endif
}

void run_server_loop_with_threadpool(int server_fd, const char *content_directory, const bool show_ext, threadpool_t *pool)
{
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        log_error_code(12, "Failed to initialize Winsock");
        return;
    }
#endif

    /* Main server loop with thread pool - accepts connections and queues them */
    while (!is_shutdown_requested())
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0)
        {
            /* EINTR means accept was interrupted by a signal - check if shutdown was requested */
            if (errno == EINTR)
            {
                if (is_shutdown_requested())
                    break;
                continue;
            }

            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "accept() failed: %s", strerror(errno));
            log_error_code(15, "%s", err_msg);
            continue;
        }

        /* Submit work to thread pool instead of handling directly */
        work_item_t work;
        work.client_fd = client_fd;
        work.client_addr = client_addr;
        work.content_directory = content_directory;
        work.show_ext = show_ext;

        threadpool_submit(pool, work);
    }

    log_info("Graceful shutdown initiated");

#ifdef _WIN32
    WSACleanup();
#endif
}