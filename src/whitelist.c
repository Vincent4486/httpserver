#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "include/whitelist.h"

/* ===== IP WHITELIST ===== */

/* Parse CIDR notation (e.g., "192.168.1.0/24") into network and mask */
static int parse_cidr(const char *cidr, uint32_t *out_network, uint32_t *out_mask)
{
    char buf[32];
    strncpy(buf, cidr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *slash = strchr(buf, '/');
    int prefix = 32;

    if (slash)
    {
        *slash = '\0';
        prefix = atoi(slash + 1);
        if (prefix < 0 || prefix > 32)
            return -1;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, buf, &addr) != 1)
        return -1;

    *out_network = ntohl(addr.s_addr);
    *out_mask = (prefix == 0) ? 0 : (0xFFFFFFFFU << (32 - prefix));

    return 0;
}

/* Check if IP matches a single whitelist entry (exact IP or CIDR range) */
static int ip_matches_entry(const char *client_ip, const char *entry)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, client_ip, &addr) != 1)
        return 0;

    uint32_t client_addr = ntohl(addr.s_addr);
    uint32_t network, mask;

    if (parse_cidr(entry, &network, &mask) != 0)
        return 0;

    return (client_addr & mask) == (network & mask) ? 1 : 0;
}

/* Check if IP address is in whitelist
   Returns 1 if IP is allowed, 0 if not allowed */
int is_ip_whitelisted(const char *client_ip, char **whitelist_ips, int count)
{
    if (!client_ip || !whitelist_ips || count <= 0)
        return 0;

    for (int i = 0; i < count; i++)
    {
        if (ip_matches_entry(client_ip, whitelist_ips[i]))
            return 1;
    }

    return 0;
}

/* ===== FILE WHITELIST ===== */

/* Check if file path matches a whitelist entry (exact match or prefix match for directories) */
static int path_matches_entry(const char *request_path, const char *entry)
{
    if (!request_path || !entry)
        return 0;

    /* Exact match */
    if (strcmp(request_path, entry) == 0)
        return 1;

    /* If entry ends with /, it's a directory - check prefix match */
    size_t entry_len = strlen(entry);
    if (entry_len > 0 && entry[entry_len - 1] == '/')
    {
        if (strncmp(request_path, entry, entry_len) == 0)
            return 1;
    }

    return 0;
}

/* Check if file path is whitelisted
   Returns 1 if file is allowed, 0 if not allowed */
int is_file_whitelisted(const char *request_path, char **whitelist_files, int count)
{
    if (!request_path || !whitelist_files || count <= 0)
        return 0;

    for (int i = 0; i < count; i++)
    {
        if (path_matches_entry(request_path, whitelist_files[i]))
            return 1;
    }

    return 0;
}