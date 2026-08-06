#ifndef PC_NETWORK_CONFIG_H
#define PC_NETWORK_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pc_network_config_s {
    int enabled;
    char host[256];
    uint16_t port;
    uint64_t town_id;
    uint64_t account_id;
    char invite_key[128];
} pc_network_config_t;

void pc_network_config_defaults(pc_network_config_t* config);
int pc_network_config_load(const char* path, pc_network_config_t* config,
                           int* file_found, char* error, int error_size);
int pc_network_config_write_default(const char* path, char* error, int error_size);
int pc_network_parse_endpoint(const char* endpoint, char* host, int host_size,
                              uint16_t* port, char* error, int error_size);

#ifdef __cplusplus
}
#endif

#endif
