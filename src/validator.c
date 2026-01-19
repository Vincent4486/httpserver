#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "include/settings.h"
#include "include/logger.h"
#include "include/validator.h"

int validate_config(void)
{
    char err_msg[256];

    /* Validate port */
    int port = get_server_port();
    if (port < 1 || port > 65535)
    {
        snprintf(err_msg, sizeof(err_msg), "Invalid port: %d (must be 1-65535)", port);
        log_error_code(100, "%s", err_msg);
        return 0;
    }

    /* Validate server directory exists */
    const char *dir = get_server_directory();
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        snprintf(err_msg, sizeof(err_msg), "Server directory not found or not a directory: %s", dir);
        log_error_code(101, "%s", err_msg);
        return 0;
    }

    /* Validate host */
    const char *host = get_server_host();
    if (!host || (strcmp(host, "any") != 0 && strcmp(host, "localhost") != 0 && strcmp(host, "127.0.0.1") != 0))
    {
        snprintf(err_msg, sizeof(err_msg), "Invalid host: %s (must be 'any', 'localhost', or '127.0.0.1')", host);
        log_error_code(102, "%s", err_msg);
        return 0;
    }

    log_info("✅ Configuration validation passed");
    return 1;
}
