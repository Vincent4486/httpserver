#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>

#include "include/metrics.h"

static struct
{
    unsigned long total_requests;
    unsigned long total_bytes;
    double total_response_time;
    double min_response_time;
    double max_response_time;
    time_t start_time;
    unsigned long current_memory_bytes;
    unsigned long peak_memory_bytes;
    double total_cpu_time_ms;
    pthread_mutex_t lock;
} metrics = {
    .total_requests = 0,
    .total_bytes = 0,
    .total_response_time = 0.0,
    .min_response_time = 1e9,
    .max_response_time = 0.0,
    .start_time = 0,
    .current_memory_bytes = 0,
    .peak_memory_bytes = 0,
    .total_cpu_time_ms = 0.0};

void metrics_init(void)
{
    pthread_mutex_init(&metrics.lock, NULL);
    metrics.start_time = time(NULL);
}

void metrics_record_request(size_t bytes_sent, double response_time_ms)
{
    pthread_mutex_lock(&metrics.lock);

    metrics.total_requests++;
    metrics.total_bytes += bytes_sent;
    metrics.total_response_time += response_time_ms;

    if (response_time_ms < metrics.min_response_time)
        metrics.min_response_time = response_time_ms;

    if (response_time_ms > metrics.max_response_time)
        metrics.max_response_time = response_time_ms;

    pthread_mutex_unlock(&metrics.lock);
}

metrics_t metrics_get(void)
{
    metrics_t snapshot;
    pthread_mutex_lock(&metrics.lock);

    snapshot.total_requests = metrics.total_requests;
    snapshot.total_bytes = metrics.total_bytes;
    snapshot.min_response_time = (metrics.total_requests > 0) ? metrics.min_response_time : 0.0;
    snapshot.max_response_time = metrics.max_response_time;
    snapshot.avg_response_time = (metrics.total_requests > 0) ? (metrics.total_response_time / metrics.total_requests) : 0.0;
    snapshot.start_time = metrics.start_time;
    snapshot.current_memory_bytes = metrics.current_memory_bytes;
    snapshot.peak_memory_bytes = metrics.peak_memory_bytes;
    snapshot.total_cpu_time_ms = metrics.total_cpu_time_ms;

    pthread_mutex_unlock(&metrics.lock);
    return snapshot;
}

unsigned long metrics_get_uptime(void)
{
    return (unsigned long)(time(NULL) - metrics.start_time);
}

void metrics_update_memory(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return;

    pthread_mutex_lock(&metrics.lock);

    /* On most systems, ru_maxrss gives peak memory in KB */
    unsigned long peak_kb = (unsigned long)usage.ru_maxrss;
    unsigned long peak_bytes = peak_kb * 1024;

    if (peak_bytes > metrics.peak_memory_bytes)
        metrics.peak_memory_bytes = peak_bytes;

    /* Calculate CPU time (user + system) in milliseconds */
    double cpu_time_ms = (usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0) +
                         (usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0);
    metrics.total_cpu_time_ms = cpu_time_ms;

    pthread_mutex_unlock(&metrics.lock);
}
