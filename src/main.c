#include <stdio.h>   // printf, perror
#include <stdlib.h>  // exit, malloc
#include <string.h>  // strlen, strcmp, strtok
#include <stdbool.h> // bool

#include "include/compat.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

#include "include/settings.h"
#include "include/socket.h"
#include "include/client.h"
#include "include/logger.h"
#include "include/validator.h"
#include "include/metrics.h"
#include "include/shutdown.h"
#include "include/access_log.h"
#include "include/threadpool.h"

/* Helper: Process command-line arguments */
static int process_arguments(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--config") == 0)
        {
            if (i + 1 < argc)
            {
                set_config_path(argv[i + 1]);
                i++;
            }
            else
            {
                fprintf(stderr, "Error: --config flag requires a path argument\n");
                return 1;
            }
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Parse command-line arguments */
    if (process_arguments(argc, argv) != 0)
        return 1;

#ifdef _WIN32
    WSADATA wsaData;
    int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsa_result != 0)
    {
        fprintf(stderr, "WSAStartup failed: %d\n", wsa_result);
        return 1;
    }
#endif

    /* Initialize signal handlers for graceful shutdown */
    init_signal_handlers();

    /* Validate configuration */
    if (!validate_config())
        return 1;

    /* Initialize metrics tracking */
    metrics_init();

    /* Initialize access logging if enabled */
    if (get_enable_access_logging())
    {
        access_log_init(get_access_log_file());
    }

    /* Initialize file cache */
    cache_init();

    const char *server_content_directory = get_server_directory();
    int server_port = get_server_port();
    const char *server_host = get_server_host();
    const bool show_file_ext = get_show_file_extension();
    int thread_pool_size = get_thread_pool_size();

    log_info("Server Directory: ");
    log_info(server_content_directory);
    char port_message[50];
    snprintf(port_message, sizeof(port_message), "Server Port: %d", server_port);
    log_info(port_message);
    log_info("Server Host: ");
    log_info(server_host);
    if (show_file_ext)
        log_info("File Extension Mode: SHOW-EXTENSION");
    else
        log_info("File Extension Mode: HIDE-EXTENSION");

    char thread_msg[64];
    snprintf(thread_msg, sizeof(thread_msg), "Thread Pool Size: %d", thread_pool_size);
    log_info(thread_msg);

    log_info("Reminder: When changed file extension mode to hide file extensions, files wit extensions will still work, please clear browser history to have the new version as default.");

    int server_fd = start_server(server_host, server_port);

    /* Create thread pool */
    threadpool_t *pool = threadpool_create(thread_pool_size);
    if (!pool)
    {
        log_error_code(18, "Failed to create thread pool");
        return 1;
    }

    run_server_loop_with_threadpool(server_fd, server_content_directory, show_file_ext, pool);

    /* Cleanup access logging */
    access_log_close();

    /* Shutdown thread pool */
    threadpool_shutdown(pool);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
