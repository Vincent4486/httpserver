#include <stdio.h>    // printf, perror
#include <stdlib.h>   // exit, EXIT_FAILURE
#include <string.h>   // strlen, strcpy, memset
#include <stdbool.h>  // bool, true, fals
#include <stdint.h>   // intmax_t
#include <inttypes.h> // PRIdMAX
#include <ctype.h>    // isxdigit
#include <limits.h>   // PATH_MAX
#include <time.h>     // time

#include "include/compat.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

#include "include/http.h"

#include "include/client.h"
#include "include/logger.h"
#include "include/whitelist.h"
#include "include/settings.h"
#include "include/gzip.h"
#include "include/metrics.h"
#include "include/access_log.h"
#include "include/shutdown.h"
#include "include/threadpool.h"
#include "include/health.h"

/* Forward declarations */
static int serve_file_cached(int client_fd, const char *file_path, const char *method,
                             const char *request_path, bool keep_alive, const char *request_buf);

/* Helper: Handle directory in SHOW-EXTENSION mode */
static int handle_show_ext_directory(int client_fd, const char *content_directory, char *path,
                                      char *candidate, const char *request_buf)
{
    size_t plen = strlen(path);
    if (path[plen - 1] != '/')
    {
        char with_slash[1024];
        snprintf(with_slash, sizeof(with_slash), "%s/", path);
        send_301_location(client_fd, with_slash);
        return 0;
    }
    
    size_t n = snprintf(candidate, PATH_MAX, "%s%sindex.html",
                        content_directory,
                        path[0] == '/' ? path + 1 : path);
    
    struct stat st;
    if (n >= PATH_MAX || stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
    {
        send_404(client_fd);
        return -1;
    }
    return 0;
}

/* Helper: Resolve file with optional .html extension in SHOW-EXTENSION mode */
static int resolve_show_ext_file(int client_fd, const char *content_directory, char *path, char *candidate)
{
    struct stat st;
    if (strrchr(path, '.') == NULL)
    {
        char alt[PATH_MAX];
        if (snprintf(alt, sizeof(alt), "%s%s.html", content_directory, path) < (int)sizeof(alt) &&
            stat(alt, &st) == 0 && S_ISREG(st.st_mode))
        {
            strncpy(candidate, alt, PATH_MAX - 1);
            candidate[PATH_MAX - 1] = '\0';
            return 0;
        }
    }
    send_404(client_fd);
    return -1;
}

static int show_ext_mode(int client_fd, const char *content_directory, const char *method,
                         char *path, bool keep_alive, const char *request_buf, const char *abs_content)
{
    if (strcmp(path, "/") == 0)
    {
        send_301_location(client_fd, "/index.html");
        return 0;
    }

    char candidate[PATH_MAX];
    if (join_path(content_directory, path, candidate, sizeof(candidate)) != 0)
    {
        send_404(client_fd);
        return -1;
    }

    struct stat st;
    if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode))
    {
        if (handle_show_ext_directory(client_fd, content_directory, path, candidate, request_buf) != 0)
            return -1;
    }
    else if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
    {
        if (resolve_show_ext_file(client_fd, content_directory, path, candidate) != 0)
            return -1;
    }

    char abs_candidate[PATH_MAX];
    if (!realpath(candidate, abs_candidate) ||
        strncmp(abs_candidate, abs_content, strlen(abs_content)) != 0 ||
        (abs_candidate[strlen(abs_content)] != '/' && abs_candidate[strlen(abs_content)] != '\0'))
    {
        send_403(client_fd);
        return -1;
    }

    return serve_file_cached(client_fd, abs_candidate, method, path, keep_alive, request_buf);
}

/* Helper: Handle root path in HIDE-EXTENSION mode */
static int handle_hide_ext_root(int client_fd, const char *content_directory, const char *method,
                                 bool keep_alive, const char *request_buf, const char *abs_content)
{
    char cand[PATH_MAX];
    if (join_path(content_directory, "/index.html", cand, sizeof(cand)) != 0)
    {
        send_404(client_fd);
        return -1;
    }
    
    struct stat st;
    if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode))
    {
        send_404(client_fd);
        return -1;
    }
    
    char abs_cand[PATH_MAX];
    if (!realpath(cand, abs_cand) ||
        strncmp(abs_cand, abs_content, strlen(abs_content)) != 0)
    {
        send_403(client_fd);
        return -1;
    }
    
    return serve_file_cached(client_fd, abs_cand, method, "/index.html", keep_alive, request_buf);
}

/* Helper: Check for .html redirect in HIDE-EXTENSION mode */
static int check_html_redirect(int client_fd, const char *content_directory, const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext && strcmp(ext, ".html") == 0)
    {
        char requested_fs[PATH_MAX];
        if (join_path(content_directory, path, requested_fs, sizeof(requested_fs)) == 0)
        {
            struct stat rst;
            if (stat(requested_fs, &rst) == 0 && S_ISREG(rst.st_mode))
            {
                size_t base_len = (size_t)(ext - path);
                char clean_path[1024];
                
                if (base_len == 0)
                    strcpy(clean_path, "/");
                else
                {
                    size_t copy_len = base_len < sizeof(clean_path) - 2 ? base_len : sizeof(clean_path) - 2;
                    memcpy(clean_path, path, copy_len);
                    clean_path[copy_len] = '\0';
                    size_t rl = strlen(clean_path);
                    if (rl > 0 && clean_path[rl - 1] != '/')
                        strcat(clean_path, "/");
                }

                char candidate_fs[PATH_MAX];
                if (snprintf(candidate_fs, sizeof(candidate_fs), "%s%sindex.html", content_directory,
                             clean_path[0] == '/' ? clean_path + 1 : clean_path) < (int)sizeof(candidate_fs))
                {
                    struct stat cst;
                    if (stat(candidate_fs, &cst) == 0 && S_ISREG(cst.st_mode))
                    {
                        send_301_location(client_fd, clean_path);
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* Helper: Resolve final path in HIDE-EXTENSION mode */
static int resolve_hide_ext_path(int client_fd, const char *content_directory, const char *method,
                                  char *path, bool keep_alive, const char *request_buf, const char *abs_content)
{
    char resolved_req[PATH_MAX];
    if (strrchr(path, '.') == NULL)
    {
        if (snprintf(resolved_req, sizeof(resolved_req), "%s.html", path) >= (int)sizeof(resolved_req))
        {
            send_404(client_fd);
            return -1;
        }
    }
    else
    {
        strncpy(resolved_req, path, sizeof(resolved_req) - 1);
        resolved_req[sizeof(resolved_req) - 1] = '\0';
    }

    char candidate_fs[PATH_MAX];
    if (join_path(content_directory, resolved_req, candidate_fs, sizeof(candidate_fs)) != 0)
    {
        send_404(client_fd);
        return -1;
    }

    char abs_candidate[PATH_MAX];
    if (!realpath(candidate_fs, abs_candidate) ||
        strncmp(abs_candidate, abs_content, strlen(abs_content)) != 0 ||
        (abs_candidate[strlen(abs_content)] != '/' && abs_candidate[strlen(abs_content)] != '\0'))
    {
        send_403(client_fd);
        return -1;
    }

    struct stat st;
    if (stat(abs_candidate, &st) != 0 || !S_ISREG(st.st_mode))
    {
        send_404(client_fd);
        return -1;
    }

    return serve_file_cached(client_fd, abs_candidate, method, resolved_req, keep_alive, request_buf);
}

static int hide_ext_mode(int client_fd, const char *content_directory, const char *method,
                         char *path, bool keep_alive, const char *request_buf, const char *abs_content)
{
    if (strcmp(path, "/") == 0)
        return handle_hide_ext_root(client_fd, content_directory, method, keep_alive, request_buf, abs_content);

    size_t plen = strlen(path);
    if (plen > 1 && path[plen - 1] == '/')
        path[plen - 1] = '\0';

    if (check_html_redirect(client_fd, content_directory, path))
        return 0;

    return resolve_hide_ext_path(client_fd, content_directory, method, path, keep_alive, request_buf, abs_content);
}

/* Helper: Check cache or serve from file */
static int serve_or_cache_file(int client_fd, int fd, const char *file_path, const char *mime,
                                off_t file_size, bool keep_alive)
{
    if (file_size <= CACHE_MAX_FILE_SIZE && file_size > 0)
    {
        char *buffer = malloc(file_size);
        if (buffer)
        {
            ssize_t bytes_read = read(fd, buffer, file_size);
            if (bytes_read == file_size)
            {
                struct stat st;
                stat(file_path, &st);
                cache_put(file_path, buffer, file_size, mime, st.st_mtime);
                
                if (write_buffer_fully(client_fd, buffer, file_size) == 0)
                {
                    free(buffer);
                    return 0;
                }
            }
            free(buffer);
        }
        lseek(fd, 0, SEEK_SET);
    }
    
    return stream_file_fd(client_fd, fd, file_size);
}

/* Helper: Handle range requests */
static int handle_range_request(int client_fd, int fd, const char *mime, off_t range_start,
                                 off_t range_end, off_t file_size, const char *method)
{
    send_206_header(client_fd, mime, range_start, range_end, file_size);
    if (strcmp(method, "HEAD") == 0)
    {
        close(fd);
        return 0;
    }
    
    lseek(fd, range_start, SEEK_SET);
    off_t range_size = range_end - range_start + 1;
    return stream_file_fd(client_fd, fd, range_size);
}

/* Helper: Check cache and serve */
static int check_and_serve_cache(int client_fd, const char *file_path, const char *method,
                                  const char *mime, bool keep_alive)
{
    cache_entry_t *cached = cache_get(file_path);
    if (!cached || strcmp(method, "HEAD") == 0)
        return -1;

    if (keep_alive)
        send_200_header_keepalive(client_fd, cached->mime_type, cached->size);
    else
        send_200_header(client_fd, cached->mime_type, cached->size);

    if (write_buffer_fully(client_fd, cached->data, cached->size) != 0)
        return -1;
    
    return 0;
}

/* Serve file with caching support, conditional requests, range requests, and gzip */
static int serve_file_cached(int client_fd, const char *file_path, const char *method,
                             const char *request_path, bool keep_alive, const char *request_buf)
{
    struct stat st;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    time_t if_modified_since = 0;
    if (get_if_modified_since(request_buf, &if_modified_since))
    {
        if (if_modified_since >= st.st_mtime)
        {
            send_304(client_fd);
            return 0;
        }
    }

    off_t file_size = st.st_size;
    const char *mime = get_mime_type(request_path);

    off_t range_start = 0, range_end = file_size - 1;
    int has_range = parse_range_header(request_buf, file_size, &range_start, &range_end);

    if (!has_range)
    {
        if (check_and_serve_cache(client_fd, file_path, method, mime, keep_alive) == 0)
            return 0;
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0)
        return -1;

    if (has_range)
        return handle_range_request(client_fd, fd, mime, range_start, range_end, file_size, method);

    if (keep_alive)
        send_200_header_keepalive(client_fd, mime, file_size);
    else
        send_200_header(client_fd, mime, file_size);

    if (strcmp(method, "HEAD") == 0)
    {
        close(fd);
        return 0;
    }

    int ret = serve_or_cache_file(client_fd, fd, file_path, mime, file_size, keep_alive);
    close(fd);
    return ret;
}

/* Helper: Validate initial request */
static int validate_request(char *method, char *path)
{
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        return 1;

    if (strstr(path, ".."))
        return 1;

    if (url_decode(path) != 0)
        return 1;

    if (path[0] != '/')
        return 1;

    return 0;
}

/* Helper: Handle invalid HTTP method */
static void handle_invalid_method(int client_fd, const char *client_ip, const char *method, const char *path)
{
    const char *not_impl =
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Allow: GET, HEAD\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    write(client_fd, not_impl, strlen(not_impl));
    access_log_request(client_ip, method, path, "HTTP/1.1", 405, 0, NULL, NULL);
}

void handle_http_request(int client_fd, const char *client_ip, const char *content_directory, bool show_ext)
{
    char buffer[16384];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
        return;
    buffer[bytes_read] = '\0';

    char method[16] = {0}, path[1024] = {0};
    if (sscanf(buffer, "%15s %1023s", method, path) != 2)
        return;

    if (get_whitelist_enabled())
        handle_whitelist(client_fd, client_ip, method, path);

    if (strcmp(path, "/health") == 0 || strcmp(path, "/status") == 0)
    {
        handle_health(client_fd, client_ip, method, path);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
    {
        handle_invalid_method(client_fd, client_ip, method, path);
        return;
    }

    bool keep_alive = (strstr(buffer, "Connection: keep-alive") != NULL ||
                       strstr(buffer, "Connection: Keep-Alive") != NULL);

    if (validate_request(method, path) != 0)
    {
        if (strstr(path, "..") || path[0] != '/')
            send_403(client_fd);
        else
            send_404(client_fd);
        return;
    }

    char abs_content[PATH_MAX];
    if (!realpath(content_directory, abs_content))
        return;

    if (show_ext)
        show_ext_mode(client_fd, content_directory, method, path, keep_alive, buffer, abs_content);
    else
        hide_ext_mode(client_fd, content_directory, method, path, keep_alive, buffer, abs_content);
}