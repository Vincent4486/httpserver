#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <string.h>

#include "include/access_log.h"
#include "include/logger.h"

static FILE *log_file_handle = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void access_log_init(const char *log_file)
{
    pthread_mutex_lock(&log_mutex);

    if (log_file_handle != NULL)
    {
        fclose(log_file_handle);
    }

    log_file_handle = fopen(log_file, "a");
    if (log_file_handle == NULL)
    {
        log_error("Failed to open access log file");
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    setbuf(log_file_handle, NULL); /* Unbuffered logging for real-time visibility */
    log_info("Access log initialized");

    pthread_mutex_unlock(&log_mutex);
}

static char *format_timestamp(char *buffer, size_t buffer_size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    /* Apache combined format: [dd/Mon/YYYY:HH:MM:SS +0000] */
    strftime(buffer, buffer_size, "[%d/%b/%Y:%H:%M:%S %z]", tm_info);
    return buffer;
}

void access_log_request(
    const char *client_ip,
    const char *method,
    const char *path,
    const char *protocol,
    int status_code,
    long bytes_sent,
    const char *referer,
    const char *user_agent)
{
    if (log_file_handle == NULL)
    {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));

    /* Handle NULL values for optional fields */
    const char *safe_referer = referer ? referer : "-";
    const char *safe_user_agent = user_agent ? user_agent : "-";

    pthread_mutex_lock(&log_mutex);

    /* Apache combined format:
     * IP - - [timestamp] "METHOD PATH PROTOCOL" STATUS BYTES "REFERER" "USER-AGENT"
     */
    fprintf(log_file_handle,
            "%s - - %s \"%s %s %s\" %d %ld \"%s\" \"%s\"\n",
            client_ip ? client_ip : "0.0.0.0",
            timestamp,
            method ? method : "UNKNOWN",
            path ? path : "/",
            protocol ? protocol : "HTTP/1.1",
            status_code,
            bytes_sent,
            safe_referer,
            safe_user_agent);

    pthread_mutex_unlock(&log_mutex);
}

void access_log_close(void)
{
    pthread_mutex_lock(&log_mutex);

    if (log_file_handle != NULL)
    {
        fclose(log_file_handle);
        log_file_handle = NULL;
        log_info("Access log closed");
    }

    pthread_mutex_unlock(&log_mutex);
}
