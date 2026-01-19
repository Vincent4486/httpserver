#include <stdio.h>    // printf, perror
#include <stdlib.h>   // exit, EXIT_FAILURE
#include <string.h>   // strlen, strcpy, memset
#include <stdbool.h>  // bool, true, false
#include <stdint.h>   // intmax_t
#include <inttypes.h> // PRIdMAX
#include <ctype.h>    // isxdigit
#include <time.h>     // time, strptime, mktime

#include "include/compat.h"
#include "include/client.h"
#include "include/logger.h"
#include "include/http.h"
#include "include/whitelist.h"
#include "include/settings.h"
#include "include/metrics.h"
#include "include/shutdown.h"
#include "include/access_log.h"
#include "include/threadpool.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

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
    handle_http_request_with_timing(client_fd, client_ip, content_directory, show_ext);
#else
    time_t start_time = time(NULL);
    int request_count = 0;

    while (1)
    {
        if (request_count > 0 && time(NULL) - start_time > 30)
            break;

        struct timeval tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char peek_buf[1];
        ssize_t peek_result = recv(client_fd, peek_buf, 1, MSG_PEEK);
        if (peek_result <= 0)
            break;

        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_http_request_with_timing(client_fd, client_ip, content_directory, show_ext);
        request_count++;

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

    while (!is_shutdown_requested())
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0)
        {
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

    while (!is_shutdown_requested())
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0)
        {
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
