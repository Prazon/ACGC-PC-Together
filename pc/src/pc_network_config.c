#include "pc_network_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* trim(char* text) {
    char* end;
    while (*text != '\0' && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static void set_error(char* error, int error_size, const char* message, int line) {
    if (error == NULL || error_size <= 0) return;
    if (line > 0) snprintf(error, (size_t)error_size, "%s at line %d", message, line);
    else snprintf(error, (size_t)error_size, "%s", message);
}

static void strip_comment(char* text) {
    int quoted = 0;
    int escaped = 0;
    while (*text != '\0') {
        if (escaped) {
            escaped = 0;
        } else if (*text == '\\' && quoted) {
            escaped = 1;
        } else if (*text == '"') {
            quoted = !quoted;
        } else if (!quoted && (*text == ';' || *text == '#')) {
            *text = '\0';
            return;
        }
        text++;
    }
}

static int copy_value(const char* source, char* destination, size_t capacity, int allow_empty) {
    size_t length;
    if (source[0] == '"') {
        const char* input = source + 1;
        size_t output = 0;
        while (*input != '\0' && *input != '"') {
            char character = *input++;
            if (character == '\\') {
                character = *input++;
                if (character != '\\' && character != '"') return 0;
            }
            if (character == '\0' || output + 1 >= capacity) return 0;
            destination[output++] = character;
        }
        if (*input != '"' || trim((char*)(input + 1))[0] != '\0') return 0;
        destination[output] = '\0';
        return allow_empty || output != 0;
    }
    length = strlen(source);
    if (length >= capacity || (!allow_empty && length == 0)) return 0;
    memcpy(destination, source, length + 1);
    return 1;
}

static int parse_u64(const char* text, uint64_t maximum, uint64_t* output) {
    char* end = NULL;
    unsigned long long value;
    if (text[0] == '\0' || text[0] == '-') return 0;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) return 0;
    *output = (uint64_t)value;
    return 1;
}

static int parse_bool(const char* text, int* output) {
    if (strcmp(text, "true") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "1") == 0) {
        *output = 1;
        return 1;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "no") == 0 || strcmp(text, "0") == 0) {
        *output = 0;
        return 1;
    }
    return 0;
}

void pc_network_config_defaults(pc_network_config_t* config) {
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->port = 24680;
    config->town_id = 1;
    config->account_id = 1001;
    memcpy(config->host, "127.0.0.1", sizeof("127.0.0.1"));
}

int pc_network_parse_endpoint(const char* endpoint, char* host, int host_size,
                              uint16_t* port, char* error, int error_size) {
    const char* host_begin = endpoint;
    const char* host_end = NULL;
    const char* port_begin = NULL;
    uint64_t parsed_port;
    size_t host_length;
    if (endpoint == NULL || endpoint[0] == '\0' || host == NULL || host_size <= 1 || port == NULL) {
        set_error(error, error_size, "server endpoint is empty", 0);
        return 0;
    }
    if (endpoint[0] == '[') {
        host_begin = endpoint + 1;
        host_end = strchr(host_begin, ']');
        if (host_end == NULL || (host_end[1] != '\0' && host_end[1] != ':')) {
            set_error(error, error_size, "invalid bracketed server address", 0);
            return 0;
        }
        if (host_end[1] == ':') port_begin = host_end + 2;
    } else {
        const char* first_colon = strchr(endpoint, ':');
        const char* last_colon = strrchr(endpoint, ':');
        if (first_colon != NULL && first_colon == last_colon) {
            host_end = first_colon;
            port_begin = first_colon + 1;
        } else {
            host_end = endpoint + strlen(endpoint);
        }
    }
    host_length = (size_t)(host_end - host_begin);
    if (host_length == 0 || host_length >= (size_t)host_size) {
        set_error(error, error_size, "server host is empty or too long", 0);
        return 0;
    }
    if (port_begin != NULL) {
        if (!parse_u64(port_begin, 65535, &parsed_port) || parsed_port == 0) {
            set_error(error, error_size, "server port is invalid", 0);
            return 0;
        }
        *port = (uint16_t)parsed_port;
    }
    memcpy(host, host_begin, host_length);
    host[host_length] = '\0';
    return 1;
}

int pc_network_config_load(const char* path, pc_network_config_t* config,
                           int* file_found, char* error, int error_size) {
    FILE* input;
    char buffer[2048];
    int line_number = 0;
    int in_connection_section = 1;
    if (file_found != NULL) *file_found = 0;
    if (path == NULL || config == NULL) {
        set_error(error, error_size, "invalid network configuration arguments", 0);
        return 0;
    }
    input = fopen(path, "rb");
    if (input == NULL) {
        if (errno == ENOENT) return 1;
        set_error(error, error_size, "cannot open network configuration", 0);
        return 0;
    }
    if (file_found != NULL) *file_found = 1;
    while (fgets(buffer, sizeof(buffer), input) != NULL) {
        char* line;
        char* separator;
        char* key;
        char* value;
        uint64_t number;
        line_number++;
        if (strchr(buffer, '\n') == NULL && !feof(input)) {
            fclose(input);
            set_error(error, error_size, "network configuration line is too long", line_number);
            return 0;
        }
        strip_comment(buffer);
        line = trim(buffer);
        if (line[0] == '\0') continue;
        if (line[0] == '[') {
            size_t length = strlen(line);
            if (length < 3 || line[length - 1] != ']') {
                fclose(input);
                set_error(error, error_size, "invalid INI section", line_number);
                return 0;
            }
            line[length - 1] = '\0';
            line = trim(line + 1);
            in_connection_section = strcmp(line, "connection") == 0 || strcmp(line, "network") == 0;
            if (!in_connection_section) {
                fclose(input);
                set_error(error, error_size, "unknown network INI section", line_number);
                return 0;
            }
            continue;
        }
        if (!in_connection_section) continue;
        separator = strchr(line, '=');
        if (separator == NULL) {
            fclose(input);
            set_error(error, error_size, "expected key = value", line_number);
            return 0;
        }
        *separator = '\0';
        key = trim(line);
        value = trim(separator + 1);
        if (strcmp(key, "enabled") == 0) {
            if (!parse_bool(value, &config->enabled)) goto invalid;
        } else if (strcmp(key, "server") == 0) {
            char endpoint[320];
            if (!copy_value(value, endpoint, sizeof(endpoint), 0) ||
                !pc_network_parse_endpoint(endpoint, config->host, sizeof(config->host),
                                           &config->port, error, error_size)) goto invalid_without_message;
        } else if (strcmp(key, "port") == 0) {
            if (!parse_u64(value, 65535, &number) || number == 0) goto invalid;
            config->port = (uint16_t)number;
        } else if (strcmp(key, "town_id") == 0) {
            if (!parse_u64(value, UINT64_MAX, &number) || number == 0) goto invalid;
            config->town_id = number;
        } else if (strcmp(key, "account_id") == 0) {
            if (!parse_u64(value, UINT64_MAX, &number) || number == 0) goto invalid;
            config->account_id = number;
        } else if (strcmp(key, "invite_key") == 0) {
            if (!copy_value(value, config->invite_key, sizeof(config->invite_key), 1)) goto invalid;
        } else {
            fclose(input);
            set_error(error, error_size, "unknown network configuration setting", line_number);
            return 0;
        }
        continue;
invalid:
        fclose(input);
        set_error(error, error_size, "invalid network configuration value", line_number);
        return 0;
invalid_without_message:
        fclose(input);
        if (error == NULL || error[0] == '\0')
            set_error(error, error_size, "invalid network configuration value", line_number);
        return 0;
    }
    if (ferror(input)) {
        fclose(input);
        set_error(error, error_size, "failed while reading network configuration", line_number);
        return 0;
    }
    fclose(input);
    return 1;
}

int pc_network_config_write_default(const char* path, char* error, int error_size) {
    FILE* output = fopen(path, "wb");
    if (output == NULL) {
        set_error(error, error_size, "cannot create default network configuration", 0);
        return 0;
    }
    fputs("; Dedicated-town client connection\r\n"
          "; Set enabled=true, use a unique persistent account_id, and keep the key private.\r\n\r\n"
          "[connection]\r\n"
          "enabled = false\r\n"
          "server = 127.0.0.1:24680\r\n"
          "town_id = 1\r\n"
          "account_id = 1001\r\n"
          "invite_key = \"\"\r\n", output);
    if (fclose(output) != 0) {
        set_error(error, error_size, "failed while writing default network configuration", 0);
        return 0;
    }
    return 1;
}
