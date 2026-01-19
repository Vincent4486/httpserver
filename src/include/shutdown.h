#ifndef SHUTDOWN_H
#define SHUTDOWN_H

#include <signal.h>
#include <stdatomic.h>

/* Initialize signal handlers for graceful shutdown */
void init_signal_handlers(void);

/* Check if shutdown signal was received */
int is_shutdown_requested(void);

#endif
