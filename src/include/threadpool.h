#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef struct threadpool threadpool_t;

/* Work item: a client connection to be handled */
typedef struct
{
    int client_fd;
    struct sockaddr_in client_addr;
    const char *content_directory;
    bool show_ext;
} work_item_t;

/* Create a thread pool with num_threads worker threads */
threadpool_t *threadpool_create(int num_threads);

/* Submit work to the thread pool (enqueue a client connection) */
void threadpool_submit(threadpool_t *pool, work_item_t work);

/* Shutdown the thread pool and wait for all workers to finish */
void threadpool_shutdown(threadpool_t *pool);

/* Get current queue size (for monitoring) */
int threadpool_queue_size(threadpool_t *pool);

#endif
