#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdatomic.h>
#include <errno.h>

#include "include/shutdown.h"
#include "include/logger.h"

static _Atomic(int) shutdown_requested = 0;

static void sigterm_handler(int sig)
{
    (void)sig; /* Avoid unused parameter warning */
    atomic_store(&shutdown_requested, 1);
}

static void sigint_handler(int sig)
{
    (void)sig;
    atomic_store(&shutdown_requested, 1);
}

void init_signal_handlers(void)
{
    struct sigaction sa;

    /* Configure SIGTERM to interrupt blocking calls */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Don't use SA_RESTART - let it interrupt accept() */
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    /* Configure SIGINT to interrupt blocking calls */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    log_info("Signal handlers initialized (SIGTERM, SIGINT)");
}

int is_shutdown_requested(void)
{
    return atomic_load(&shutdown_requested);
}
