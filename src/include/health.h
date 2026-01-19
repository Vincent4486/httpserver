/* Health monitor system */
#ifndef HEALTH_H
#define HEALTH_H

void handle_health(int client_fd, const char *client_ip, const char *method, const char *path);

#endif