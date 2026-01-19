#include <stdio.h>    // snprintf
#include <string.h>   // strcmp
#include <unistd.h>   // write

#include "include/compat.h"
#include "include/health.h"
#include "include/metrics.h"
#include "include/access_log.h"

void handle_health(int client_fd, const char *client_ip, const char *method, const char *path){
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
}