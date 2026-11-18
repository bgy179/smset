#include "sms_route_command.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *skip_space(char *text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static char *trim_space(char *text) {
    char *start = skip_space(text);
    size_t length = strlen(start);
    while (length > 0 && isspace((unsigned char)start[length - 1])) {
        start[--length] = '\0';
    }
    return start;
}

static char *next_delimited(char **cursor, char delimiter) {
    char *start;
    char *separator;

    if (cursor == NULL || *cursor == NULL) return NULL;
    start = *cursor;
    separator = strchr(start, delimiter);
    if (separator != NULL) {
        *separator = '\0';
        *cursor = separator + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static char *take_token(char **cursor, char *token, size_t token_size) {
    char *source = skip_space(*cursor);
    char quote = '\0';
    size_t length = 0;

    if (*source == '\'' || *source == '"') {
        quote = *source++;
    }

    while (*source != '\0') {
        if (quote != '\0') {
            if (*source == quote) {
                source++;
                if (*source != '\0' && !isspace((unsigned char)*source)) {
                    return NULL;
                }
                break;
            }
        } else if (isspace((unsigned char)*source)) {
            break;
        }

        if (*source == '\0' || length + 1 >= token_size) {
            return NULL;
        }
        token[length++] = *source++;
    }

    if (quote != '\0' && source[-1] != quote) {
        return NULL;
    }
    token[length] = '\0';
    *cursor = source;
    return length > 0 ? token : NULL;
}

static int valid_ipv4(const char *text) {
    char copy[SMS_ROUTE_IP_TEXT_MAX];
    char *cursor;
    char *part;
    int part_count = 0;

    if (text == NULL || strlen(text) >= sizeof(copy)) return 0;
    strcpy(copy, text);
    cursor = copy;

    while ((part = next_delimited(&cursor, '.')) != NULL) {
        char *end = NULL;
        long value;
        if (*part == '\0' || part_count >= 4) return 0;
        value = strtol(part, &end, 10);
        if (*end != '\0' || value < 0 || value > 255) return 0;
        part_count++;
    }
    return part_count == 4;
}

static int append_ip(struct sms_route_command *command, const char *ip) {
    if (!valid_ipv4(ip) || command->ip_count >= SMS_ROUTE_IP_MAX) return 0;
    snprintf(command->ips[command->ip_count],
             sizeof(command->ips[command->ip_count]), "%s", ip);
    command->ip_count++;
    return 1;
}

static int append_ip_token(struct sms_route_command *command, char *token) {
    char *hyphen = strrchr(token, '-');
    char *comma;

    while ((comma = strchr(token, ',')) != NULL || (comma = strchr(token, ';')) != NULL) {
        *comma = '\0';
        if (!append_ip_token(command, token)) return 0;
        token = comma + 1;
        if (*token == '\0') return 1;
    }

    if (!hyphen) {
        return append_ip(command, token);
    }

    char prefix[SMS_ROUTE_IP_TEXT_MAX];
    char *last_dot = strrchr(token, '.');
    char *end_start = NULL;
    char *end_end = NULL;
    long start;
    long end;
    if (last_dot == NULL || hyphen <= last_dot) return 0;

    size_t prefix_length = (size_t)(last_dot - token);
    if (prefix_length == 0 || prefix_length >= sizeof(prefix)) return 0;
    memcpy(prefix, token, prefix_length);
    prefix[prefix_length] = '\0';

    *hyphen = '\0';
    start = strtol(last_dot + 1, &end_start, 10);
    end = strtol(hyphen + 1, &end_end, 10);
    if (*end_start != '\0' || *end_end != '\0' ||
        start < 0 || start > 255 || end < start || end > 255) {
        return 0;
    }
    for (long value = start; value <= end; value++) {
        char ip[SMS_ROUTE_IP_TEXT_MAX];
        snprintf(ip, sizeof(ip), "%s.%ld", prefix, value);
        if (!append_ip(command, ip)) return 0;
    }
    return 1;
}

enum sms_route_command_type sms_route_command_parse(
    const char *message,
    struct sms_route_command *command) {
    char buffer[2048];
    char verb[32];
    char token[1024];
    char *cursor;

    if (command == NULL || message == NULL) return SMS_ROUTE_COMMAND_NONE;
    memset(command, 0, sizeof(*command));
    snprintf(buffer, sizeof(buffer), "%s", message);
    cursor = trim_space(buffer);
    if (!take_token(&cursor, verb, sizeof(verb))) {
        return SMS_ROUTE_COMMAND_NONE;
    }

    if (strcmp(verb, "_smsRoute") == 0) {
        if (!take_token(&cursor, command->nickname, sizeof(command->nickname))) {
            command->type = SMS_ROUTE_COMMAND_INVALID;
            return command->type;
        }
        if (*skip_space(cursor) != '\0') {
            command->type = SMS_ROUTE_COMMAND_INVALID;
            return command->type;
        }
        command->type = SMS_ROUTE_COMMAND_ROUTE;
        return command->type;
    }

    if (strcmp(verb, "_smsRegister") != 0) {
        return SMS_ROUTE_COMMAND_NONE;
    }

    if (!take_token(&cursor, command->nickname, sizeof(command->nickname))) {
        command->type = SMS_ROUTE_COMMAND_INVALID;
        return command->type;
    }

    while (*skip_space(cursor) != '\0') {
        cursor = skip_space(cursor);
        if (!take_token(&cursor, token, sizeof(token))) {
            command->type = SMS_ROUTE_COMMAND_INVALID;
            command->ip_count = 0;
            return command->type;
        }
        if (!append_ip_token(command, token)) {
            command->type = SMS_ROUTE_COMMAND_INVALID;
            command->ip_count = 0;
            return command->type;
        }
    }

    command->type = command->ip_count > 0
        ? SMS_ROUTE_COMMAND_REGISTER
        : SMS_ROUTE_COMMAND_INVALID;
    return command->type;
}

int sms_route_sender_allowed(const char *sender, const char *allowed_senders) {
    if (sender == NULL || allowed_senders == NULL || *allowed_senders == '\0') {
        return 0;
    }

    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", allowed_senders);
    char *cursor = copy;
    char *entry = next_delimited(&cursor, ',');
    while (entry != NULL) {
        if (strcmp(sender, trim_space(entry)) == 0) return 1;
        entry = next_delimited(&cursor, ',');
    }
    return 0;
}
