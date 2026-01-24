#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "lib/cJSON.h"
#include "include/compat.h"

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
static char custom_config_path[1024] = {0};

void set_config_path(const char *path)
{
    if (path)
        strncpy(custom_config_path, path, sizeof(custom_config_path) - 1);
}

static bool directory_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* Extract directory from a file path, handling both / and \ separators */
static void get_directory_from_path(const char *filepath, char *dir_out, size_t dir_size)
{
    strncpy(dir_out, filepath, dir_size - 1);
    dir_out[dir_size - 1] = '\0';
    
    /* Find last separator (either / or \) */
    char *last_sep = NULL;
    char *last_slash = strrchr(dir_out, '/');
    char *last_backslash = strrchr(dir_out, '\\');
    
    if (last_slash && last_backslash)
        last_sep = (last_slash > last_backslash) ? last_slash : last_backslash;
    else if (last_slash)
        last_sep = last_slash;
    else if (last_backslash)
        last_sep = last_backslash;
    
    if (last_sep)
        *last_sep = '\0';
    else
        strcpy(dir_out, "."); /* No separator found, use current directory */
}

char *get_config_path()
{
    if (custom_config_path[0] != '\0')
        return custom_config_path;

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
        char *config_file_path = get_config_path();
        static char executable_dir[1024];
        get_directory_from_path(config_file_path, executable_dir, sizeof(executable_dir));

        static char default_dir[1024];
        snprintf(default_dir, sizeof(default_dir), "%s%cserver-content", 
                 executable_dir, PATH_SEPARATOR);

        if (!directory_exists(default_dir))
        {
#ifdef _WIN32
            if (_mkdir(default_dir) != 0)
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
    const char *log_path = cJSON_IsString(log_file) && log_file->valuestring != NULL
                               ? log_file->valuestring
                               : "log/access.log"; /* Default path */

    /* If path is absolute, return as-is */
#ifdef _WIN32
    if ((log_path[0] != '\0' && log_path[1] == ':') || /* C:\ style */
        log_path[0] == '\\' || log_path[0] == '/')     /* \path or /path */
#else
    if (log_path[0] == '/')
#endif
    {
        return log_path;
    }

    /* For relative paths, resolve relative to config directory */
    static char resolved_log_path[1024];
    char *config_file_path = get_config_path();
    static char config_dir[1024];
    get_directory_from_path(config_file_path, config_dir, sizeof(config_dir));

    snprintf(resolved_log_path, sizeof(resolved_log_path), "%s%c%s",
             config_dir, PATH_SEPARATOR, log_path);

    /* Create log directory if it doesn't exist */
    static char log_dir[1024];
    get_directory_from_path(resolved_log_path, log_dir, sizeof(log_dir));

    struct stat st = {0};
    if (stat(log_dir, &st) == -1)
    {
#ifdef _WIN32
        if (_mkdir(log_dir) != 0)
#else
        if (mkdir(log_dir, 0700) != 0)
#endif
        {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "Failed to create log directory: %s", log_dir);
            log_error_code(22, "%s", err_msg);
            /* Continue anyway - fopen will fail with a clearer message */
        }
    }

    return resolved_log_path;
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
