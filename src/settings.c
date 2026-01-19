#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

#include "lib/cJSON.h"

#include "include/settings.h"
#include "include/logger.h"

/* If S_ISDIR/S_ISREG are not available on this platform, provide small fallbacks
    locally to avoid pulling in Windows socket headers here. */
#ifndef S_ISDIR
#ifdef _S_IFDIR
#define S_ISDIR(m) (((m) & _S_IFDIR) == _S_IFDIR)
#else
#define S_ISDIR(m) 0
#endif
#endif
#ifndef S_ISREG
#ifdef _S_IFREG
#define S_ISREG(m) (((m) & _S_IFREG) == _S_IFREG)
#else
#define S_ISREG(m) 0
#endif
#endif

static cJSON *cached_config = NULL;

static bool directory_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

char *get_config_path()
{
    static char path[1024];
    static char config_path[1024];

#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, path, sizeof(path)))
    {
        log_error_code(16, "Failed to get executable path"); /* #016 */
        exit(EXIT_FAILURE);
    }
    char *last_backslash = strrchr(path, '\\');
    if (last_backslash)
        *last_backslash = '\0';
#elif defined(__APPLE__)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0)
    {
        log_error_code(17, "Executable path too long"); /* #017 */
        exit(EXIT_FAILURE);
    }
    char *last_slash = strrchr(path, '/');
    if (last_slash)
        *last_slash = '\0';
#else
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len == -1)
    {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "readlink failed: %s", strerror(errno));
        log_error_code(8, "%s", err_msg); /* #008 */
        exit(EXIT_FAILURE);
    }
    path[len] = '\0';
    char *last_slash = strrchr(path, '/');
    if (last_slash)
        *last_slash = '\0';
#endif

    snprintf(config_path, sizeof(config_path), "%s/config.json", path);
    return config_path;
}

static void load_config()
{
    if (cached_config)
        return;

    char *filepath = get_config_path();
    FILE *file = fopen(filepath, "r");
    if (!file)
    {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to open config.json: %s", strerror(errno));
        log_error_code(9, "%s", err_msg); /* #009 */
        exit(EXIT_FAILURE);
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    rewind(file);

    char *data = malloc(length + 1);
    if (!data)
    {
        log_error_code(11, "Memory allocation failed while reading config"); /* #011 */
        fclose(file);
        exit(EXIT_FAILURE);
    }

    fread(data, 1, length, file);
    data[length] = '\0';
    fclose(file);

    cached_config = cJSON_Parse(data);
    free(data);

    if (!cached_config)
    {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Error parsing JSON: %s", cJSON_GetErrorPtr());
        log_error_code(10, "%s", err_msg); /* #010 */
        exit(EXIT_FAILURE);
    }
}

const int get_server_port()
{
    load_config();
    cJSON *port = cJSON_GetObjectItemCaseSensitive(cached_config, "server-port");
    if (!cJSON_IsNumber(port))
    {
        log_error_code(2, "Port not found or not a number in config.json"); /* #002 */
        exit(EXIT_FAILURE);
    }
    return port->valueint;
}

const char *get_server_directory()
{
    load_config();
    cJSON *dir = cJSON_GetObjectItemCaseSensitive(cached_config, "server-content-directory");
    if (!cJSON_IsString(dir) || dir->valuestring == NULL)
    {
        log_error_code(3, "Directory not found or not a string in config.json"); /* #003 */
        exit(EXIT_FAILURE);
    }

    const char *path = dir->valuestring;
    if (strcmp(path, "default") == 0)
    {
        char *executable_dir = get_config_path();
        char *last_slash = strrchr(executable_dir, '/');
        if (last_slash)
            *last_slash = '\0';

        static char default_dir[1024];
        snprintf(default_dir, sizeof(default_dir), "%s/server-content", executable_dir);

        if (!directory_exists(default_dir))
        {
#ifdef _WIN32
            if (mkdir(default_dir) != 0)
#else
            if (mkdir(default_dir, 0700) != 0)
#endif
            {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Failed to create default directory: %s", default_dir);
                log_error_code(14, "%s", err_msg); /* #014 */
                exit(EXIT_FAILURE);
            }
            else
            {
                char success_msg[256];
                snprintf(success_msg, sizeof(success_msg), "Default directory created: %s", default_dir);
                log_info(success_msg);
            }
        }

        return strdup(default_dir);
    }
    else if (!directory_exists(path))
    {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Directory does not exist: %s", path);
        log_error_code(1, "%s", err_msg); /* #001 */
        exit(EXIT_FAILURE);
    }

    return strdup(path);
}

const char *get_server_host()
{
    load_config();
    cJSON *host = cJSON_GetObjectItemCaseSensitive(cached_config, "server-host");
    if (!cJSON_IsString(host) || host->valuestring == NULL)
    {
        log_error_code(4, "Host not found or not a string in config.json"); /* #004 */
        exit(EXIT_FAILURE);
    }
    return strdup(host->valuestring);
}

const bool get_show_file_extension()
{
    load_config();
    cJSON *show_ext = cJSON_GetObjectItemCaseSensitive(cached_config, "show-file-extension");
    return cJSON_IsBool(show_ext) ? (show_ext->valueint != 0) : false;
}

const bool get_whitelist_enabled()
{
    load_config();
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(cached_config, "whitelist-enabled");
    return cJSON_IsBool(enabled) ? (enabled->valueint != 0) : false;
}

char **get_whitelist_ips(int *out_count)
{
    if (!out_count)
        return NULL;

    load_config();
    cJSON *ips = cJSON_GetObjectItemCaseSensitive(cached_config, "whitelist-ips");

    *out_count = 0;
    if (!cJSON_IsArray(ips))
        return NULL;

    int count = cJSON_GetArraySize(ips);
    if (count <= 0)
        return NULL;

    char **entries = malloc(count * sizeof(char *));
    if (!entries)
        return NULL;

    int valid_count = 0;
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(ips, i);
        if (cJSON_IsString(item) && item->valuestring)
        {
            entries[valid_count] = strdup(item->valuestring);
            if (entries[valid_count])
                valid_count++;
        }
    }

    *out_count = valid_count;
    return valid_count > 0 ? entries : NULL;
}

char **get_whitelist_files(int *out_count)
{
    if (!out_count)
        return NULL;

    load_config();
    cJSON *files = cJSON_GetObjectItemCaseSensitive(cached_config, "whitelist-files");

    *out_count = 0;
    if (!cJSON_IsArray(files))
        return NULL;

    int count = cJSON_GetArraySize(files);
    if (count <= 0)
        return NULL;

    char **entries = malloc(count * sizeof(char *));
    if (!entries)
        return NULL;

    int valid_count = 0;
    for (int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_GetArrayItem(files, i);
        if (cJSON_IsString(item) && item->valuestring)
        {
            entries[valid_count] = strdup(item->valuestring);
            if (entries[valid_count])
                valid_count++;
        }
    }

    *out_count = valid_count;
    return valid_count > 0 ? entries : NULL;
}
void free_whitelist_entries(char **entries, int count)
{
    if (!entries)
        return;
    for (int i = 0; i < count; i++)
        free(entries[i]);
    free(entries);
}

const char *get_access_log_file(void)
{
    load_config();
    cJSON *log_file = cJSON_GetObjectItemCaseSensitive(cached_config, "access-log-file");
    if (!cJSON_IsString(log_file) || log_file->valuestring == NULL)
    {
        return "log/access.log"; /* Default path */
    }
    return log_file->valuestring;
}

const bool get_enable_access_logging(void)
{
    load_config();
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(cached_config, "enable-access-logging");
    return cJSON_IsBool(enabled) ? (enabled->valueint != 0) : false;
}

int get_thread_pool_size(void)
{
    load_config();
    cJSON *size = cJSON_GetObjectItemCaseSensitive(cached_config, "thread-pool-size");
    if (cJSON_IsNumber(size) && size->valueint > 0)
    {
        return size->valueint;
    }
    return 4; /* Default to 4 threads */
}
