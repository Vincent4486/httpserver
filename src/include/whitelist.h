#ifndef WHITELIST_H
#define WHITELIST_H

#include <stdbool.h>

/* Check if IP address is in whitelist
   Returns 1 if IP is allowed, 0 if not allowed */
int is_ip_whitelisted(const char *client_ip, char **whitelist_ips, int count);

/* Check if file path is whitelisted
   Returns 1 if file is allowed, 0 if not allowed */
int is_file_whitelisted(const char *request_path, char **whitelist_files, int count);

/* Handle whitelist check */
void handle_whitelist(int client_fd, const char *client_ip, const char *method, const char *path);

#endif
