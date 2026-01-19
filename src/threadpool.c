#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "include/compat.h"
#include "include/threadpool.h"
#include "include/logger.h"
#include "include/client.h"

#define MAX_QUEUE_SIZE 256

typedef struct
{
    work_item_t work;
    struct queue_node *next;
} queue_node_t;

typedef struct threadpool
{
    pthread_t *threads;
    int num_threads;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    queue_node_t *queue_head;
    queue_node_t *queue_tail;
    int queue_size;
    bool shutdown;
} threadpool_t;

static void *worker_thread(void *arg)
{
    threadpool_t *pool = (threadpool_t *)arg;

    while (1)
    {
        pthread_mutex_lock(&pool->lock);

        /* Wait for work or shutdown signal */
        while (pool->queue_size == 0 && !pool->shutdown)
        {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }

        /* Check if we should exit */
        if (pool->shutdown && pool->queue_size == 0)
        {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        /* Dequeue work */
        if (pool->queue_size > 0)
        {
            queue_node_t *node = pool->queue_head;
            work_item_t work = node->work;
            pool->queue_head = node->next;
            pool->queue_size--;

            if (pool->queue_size == 0)
                pool->queue_tail = NULL;

            pthread_mutex_unlock(&pool->lock);
            free(node);

            /* Handle the client outside of lock */
            handle_accepted_client(work.client_fd, work.client_addr, work.content_directory, work.show_ext);
        }
        else
        {
            pthread_mutex_unlock(&pool->lock);
        }
    }

    return NULL;
}

threadpool_t *threadpool_create(int num_threads)
{
    threadpool_t *pool = malloc(sizeof(threadpool_t));
    if (!pool)
        return NULL;

    pool->num_threads = num_threads;
    pool->threads = malloc(num_threads * sizeof(pthread_t));
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->queue_size = 0;
    pool->shutdown = false;

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);

    /* Create worker threads */
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
        {
            log_error_code(19, "Failed to create worker thread");
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Thread pool created with %d workers", num_threads);
    log_info(msg);

    return pool;
}

void threadpool_submit(threadpool_t *pool, work_item_t work)
{
    if (!pool)
        return;

    queue_node_t *node = malloc(sizeof(queue_node_t));
    if (!node)
    {
        log_error("Failed to allocate work item");
        close(work.client_fd);
        return;
    }

    node->work = work;
    node->next = NULL;

    pthread_mutex_lock(&pool->lock);

    /* Check queue size limit */
    if (pool->queue_size >= MAX_QUEUE_SIZE)
    {
        log_error_code(20, "Work queue full, rejecting connection");
        pthread_mutex_unlock(&pool->lock);
        free(node);
        close(work.client_fd);
        return;
    }

    if (pool->queue_tail)
        pool->queue_tail->next = node;
    else
        pool->queue_head = node;

    pool->queue_tail = node;
    pool->queue_size++;

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

void threadpool_shutdown(threadpool_t *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->lock);

    /* Wait for all threads to finish */
    for (int i = 0; i < pool->num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    /* Cleanup remaining queued items */
    queue_node_t *node = pool->queue_head;
    while (node)
    {
        queue_node_t *next = node->next;
        close(node->work.client_fd);
        free(node);
        node = next;
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    free(pool->threads);
    free(pool);

    log_info("Thread pool shutdown complete");
}

int threadpool_queue_size(threadpool_t *pool)
{
    if (!pool)
        return 0;

    pthread_mutex_lock(&pool->lock);
    int size = pool->queue_size;
    pthread_mutex_unlock(&pool->lock);

    return size;
}
