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

/* Serve file with caching support, conditional requests, range requests, and gzip */
static int serve_file_cached(int client_fd, const char *file_path, const char *method,
                             const char *request_path, bool keep_alive, const char *request_buf)
{
    struct stat st;
    if (stat(file_path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;

    /* Check If-Modified-Since header */
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

    /* Check for Range request */
    off_t range_start = 0, range_end = file_size - 1;
    int has_range = parse_range_header(request_buf, file_size, &range_start, &range_end);

    /* Check cache first for full file */
    if (!has_range)
    {
        cache_entry_t *cached = cache_get(file_path);
        if (cached && strcmp(method, "HEAD") != 0)
        {
            /* Serve from cache */
            if (keep_alive)
                send_200_header_keepalive(client_fd, cached->mime_type, cached->size);
            else
                send_200_header(client_fd, cached->mime_type, cached->size);

            if (write_buffer_fully(client_fd, cached->data, cached->size) != 0)
                return -1;
            return 0;
        }
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0)
        return -1;

    /* Send appropriate headers */
    if (has_range)
    {
        send_206_header(client_fd, mime, range_start, range_end, file_size);
        if (strcmp(method, "HEAD") == 0)
        {
            close(fd);
            return 0;
        }
        /* Seek to range start and stream range */
        lseek(fd, range_start, SEEK_SET);
        off_t range_size = range_end - range_start + 1;
        return stream_file_fd(client_fd, fd, range_size);
    }

    /* Full file response */
    if (keep_alive)
        send_200_header_keepalive(client_fd, mime, file_size);
    else
        send_200_header(client_fd, mime, file_size);

    if (strcmp(method, "HEAD") == 0)
    {
        close(fd);
        return 0;
    }

    /* Try to cache small files */
    if (file_size <= CACHE_MAX_FILE_SIZE && file_size > 0)
    {
        char *buffer = malloc(file_size);
        if (buffer)
        {
            ssize_t bytes_read = read(fd, buffer, file_size);
            if (bytes_read == file_size)
            {
                cache_put(file_path, buffer, file_size, mime, st.st_mtime);
                if (write_buffer_fully(client_fd, buffer, file_size) == 0)
                {
                    free(buffer);
                    close(fd);
                    return 0;
                }
            }
            free(buffer);
        }
        /* Fall back to streaming if caching failed */
        lseek(fd, 0, SEEK_SET);
    }

    int ret = stream_file_fd(client_fd, fd, file_size);
    close(fd);
    return ret;
}

void handle_http_request(int client_fd, const char *client_ip, const char *content_directory, bool show_ext)
{
    char buffer[16384]; // Increased buffer size for better performance (16KB)
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0)
        return;
    buffer[bytes_read] = '\0';

    char method[16] = {0}, path[1024] = {0};
    if (sscanf(buffer, "%15s %1023s", method, path) != 2)
        return;

    /* Check file whitelist if enabled */
    if (get_whitelist_enabled())
    {
        int file_count = 0;
        char **whitelist_files = get_whitelist_files(&file_count);

        if (file_count > 0 && !is_file_whitelisted(path, whitelist_files, file_count))
        {
            send_403(client_fd);
            access_log_request(client_ip, method, path, "HTTP/1.1", 403, 0, NULL, NULL);
            free_whitelist_entries(whitelist_files, file_count);
            return;
        }

        if (whitelist_files)
            free_whitelist_entries(whitelist_files, file_count);
    }

    /* Health check endpoint */
    if (strcmp(path, "/health") == 0 || strcmp(path, "/status") == 0)
    {
        metrics_update_memory(); /* Update memory stats */
        metrics_t m = metrics_get();
        char json_response[768];
        int len = snprintf(json_response, sizeof(json_response),
                           "{"
                           "\"status\":\"ok\","
                           "\"uptime\":%lu,"
                           "\"requests\":%lu,"
                           "\"bytes_served\":%lu,"
                           "\"avg_response_time_ms\":%.2f,"
                           "\"peak_memory_kb\":%lu,"
                           "\"cpu_time_ms\":%.2f"
                           "}",
                           metrics_get_uptime(),
                           m.total_requests,
                           m.total_bytes,
                           m.avg_response_time,
                           m.peak_memory_bytes / 1024,
                           m.total_cpu_time_ms);

        char header[256];
        int header_len = snprintf(header, sizeof(header),
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: %d\r\n"
                                  "\r\n",
                                  len);

        if (header_len > 0)
            write(client_fd, header, header_len);
        if (len > 0)
            write(client_fd, json_response, len);

        access_log_request(client_ip, method, path, "HTTP/1.1", 200, len, NULL, NULL);
        return;
    }

    /* Only handle GET and HEAD */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
    {
        const char *not_impl =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET, HEAD\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        write(client_fd, not_impl, strlen(not_impl));
        access_log_request(client_ip, method, path, "HTTP/1.1", 405, 0, NULL, NULL);
        return;
    }

    /* Check for Connection: keep-alive header */
    bool keep_alive = (strstr(buffer, "Connection: keep-alive") != NULL ||
                       strstr(buffer, "Connection: Keep-Alive") != NULL);

    /* Basic reject for raw traversal tokens before decoding */
    if (strstr(path, ".."))
    {
        send_403(client_fd);
        return;
    }

    /* URL-decode path in-place */
    if (url_decode(path) != 0)
    {
        send_404(client_fd);
        return;
    }

    /* ensure path starts with '/' */
    if (path[0] != '/')
    {
        send_404(client_fd);
        return;
    }

    /* canonicalize content_directory absolute path */
    char abs_content[PATH_MAX];
    if (!realpath(content_directory, abs_content))
        return;

    /* SHOW-EXTENSION mode */
    if (show_ext)
    {
        /* redirect root to /index.html */
        if (strcmp(path, "/") == 0)
        {
            send_301_location(client_fd, "/index.html");
            return;
        }

        /* candidate filesystem path (literal) */
        char candidate[PATH_MAX];
        if (join_path(content_directory, path, candidate, sizeof(candidate)) != 0)
        {
            send_404(client_fd);
            return;
        }

        /* If candidate is a directory, try index.html inside it */
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode))
        {
            /* ensure trailing slash in URL form; redirect to slash-form if absent */
            size_t plen = strlen(path);
            if (path[plen - 1] != '/')
            {
                char with_slash[1024];
                snprintf(with_slash, sizeof(with_slash), "%s/", path);
                send_301_location(client_fd, with_slash);
                return;
            }
            /* append index.html and try that */
            size_t n = snprintf(candidate, sizeof(candidate), "%s%sindex.html",
                                content_directory,
                                path[0] == '/' ? path + 1 : path);
            if (n >= sizeof(candidate) || stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
            {
                send_404(client_fd);
                return;
            }
        }
        else if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode))
        {
            /* If literal path not found, try appending .html */
            if (strrchr(path, '.') == NULL)
            {
                char alt[PATH_MAX];
                if (snprintf(alt, sizeof(alt), "%s%s.html", content_directory, path) < (int)sizeof(alt) &&
                    stat(alt, &st) == 0 && S_ISREG(st.st_mode))
                {
                    strncpy(candidate, alt, sizeof(candidate) - 1);
                    candidate[sizeof(candidate) - 1] = '\0';
                }
                else
                {
                    send_404(client_fd);
                    return;
                }
            }
            else
            {
                send_404(client_fd);
                return;
            }
        }

        /* canonicalize candidate and ensure it's under content_directory */
        char abs_candidate[PATH_MAX];
        if (!realpath(candidate, abs_candidate) ||
            strncmp(abs_candidate, abs_content, strlen(abs_content)) != 0 ||
            (abs_candidate[strlen(abs_content)] != '/' && abs_candidate[strlen(abs_content)] != '\0'))
        {
            send_403(client_fd);
            return;
        }

        serve_file_cached(client_fd, abs_candidate, method, path, keep_alive, buffer);
        return;
    }

    /* HIDE-EXTENSION mode */
    if (strcmp(path, "/") == 0)
    {
        char cand[PATH_MAX];
        if (join_path(content_directory, "/index.html", cand, sizeof(cand)) != 0)
        {
            send_404(client_fd);
            return;
        }
        struct stat st;
        if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode))
        {
            send_404(client_fd);
            return;
        }
        char abs_cand[PATH_MAX];
        if (!realpath(cand, abs_cand) ||
            strncmp(abs_cand, abs_content, strlen(abs_content)) != 0)
        {
            send_403(client_fd);
            return;
        }
        serve_file_cached(client_fd, abs_cand, method, "/index.html", keep_alive, buffer);
        return;
    }

    /* Normalize path: strip trailing slash */
    size_t plen = strlen(path);
    if (plen > 1 && path[plen - 1] == '/')
        path[plen - 1] = '\0';

    /* If request explicitly ends with .html, check if clean path should redirect */
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
                        return;
                    }
                }
            }
        }
    }

    /* Build final filesystem path */
    char resolved_req[PATH_MAX];
    if (strrchr(path, '.') == NULL)
    {
        if (snprintf(resolved_req, sizeof(resolved_req), "%s.html", path) >= (int)sizeof(resolved_req))
        {
            send_404(client_fd);
            return;
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
        return;
    }

    /* Security check: ensure path is inside content_directory */
    char abs_candidate[PATH_MAX];
    if (!realpath(candidate_fs, abs_candidate) ||
        strncmp(abs_candidate, abs_content, strlen(abs_content)) != 0 ||
        (abs_candidate[strlen(abs_content)] != '/' && abs_candidate[strlen(abs_content)] != '\0'))
    {
        send_403(client_fd);
        return;
    }

    struct stat st;
    if (stat(abs_candidate, &st) != 0 || !S_ISREG(st.st_mode))
    {
        send_404(client_fd);
        return;
    }

    serve_file_cached(client_fd, abs_candidate, method, resolved_req, keep_alive, buffer);
}