// settings.h
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

/* Set custom config file path (must be called before any get_* functions) */
void set_config_path(const char *path);

const char *get_server_directory();   // ✅ correct declaration
const int get_server_port();          // ✅ correct declaration
const char *get_server_host();        // ✅ correct declaration
const bool get_show_file_extension(); // ✅ correct declaration

/* Whitelist configuration accessors */
const bool get_whitelist_enabled(void);
/* Returns a malloc'd array of malloc'd strings. Caller must free via free_whitelist_entries().
   On return, *out_count is set to the number of entries (may be 0). */
char **get_whitelist_ips(int *out_count);
char **get_whitelist_files(int *out_count);
void free_whitelist_entries(char **entries, int count);

/* Access logging configuration accessors */
const char *get_access_log_file(void);
const bool get_enable_access_logging(void);

/* Thread pool configuration */
int get_thread_pool_size(void);

#endif