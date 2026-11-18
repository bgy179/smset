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

static int text_equal_ignore_case(const char *left, const char *right) {
    if (left == NULL || right == NULL) return 0;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
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
    for (size_t i = 0; i < command->ip_count; i++) {
        if (strcmp(command->ips[i], ip) == 0) return 1;
    }
    snprintf(command->ips[command->ip_count],
             sizeof(command->ips[command->ip_count]), "%s", ip);
    command->ip_count++;
    return 1;
}

static int append_ip_token(struct sms_route_command *command, char *token) {
    char *comma;

    while ((comma = strchr(token, ',')) != NULL || (comma = strchr(token, ';')) != NULL) {
        *comma = '\0';
        if (!append_ip_token(command, token)) return 0;
        token = comma + 1;
        if (*token == '\0') return 1;
    }

    char prefix[SMS_ROUTE_IP_TEXT_MAX];
    char *suffix;
    char *cursor = token;
    long prefix_octets[3] = {0};
    int prefix_parts = 0;
    int valid_prefix = 1;

    for (char *dot = strchr(cursor, '.'); dot != NULL; dot = strchr(cursor, '.')) {
        char *end = NULL;
        long value;
        *dot = '\0';
        value = strtol(cursor, &end, 10);
        if (*cursor == '\0' || *end != '\0' || value < 0 || value > 255) {
            valid_prefix = 0;
            break;
        }
        prefix_octets[prefix_parts] = value;
        cursor = dot + 1;
        prefix_parts++;
        if (prefix_parts == 3) {
            suffix = cursor;
            break;
        }
    }

    if (!valid_prefix || prefix_parts != 3 || suffix == NULL || *suffix == '\0') {
        return 0;
    }

    snprintf(prefix, sizeof(prefix), "%ld.%ld.%ld",
             prefix_octets[0], prefix_octets[1], prefix_octets[2]);

    char *section = suffix;
    while (section != NULL) {
        char *plus = strchr(section, '+');
        if (plus != NULL) *plus = '\0';

        char *hyphen = strchr(section, '-');
        char *end_start = NULL;
        char *end_end = NULL;
        long start;
        long end;
        if (hyphen != NULL) {
            *hyphen = '\0';
            start = strtol(section, &end_start, 10);
            end = strtol(hyphen + 1, &end_end, 10);
            if (*section == '\0' || *end_start != '\0' || *end_end != '\0' ||
                start < 0 || end > 255 || start > end) {
                return 0;
            }
        } else {
            start = strtol(section, &end_start, 10);
            end = start;
            if (*section == '\0' || *end_start != '\0' ||
                start < 0 || start > 255) {
                return 0;
            }
        }

        for (long value = start; value <= end; value++) {
            char ip[SMS_ROUTE_IP_TEXT_MAX];
            snprintf(ip, sizeof(ip), "%s.%ld", prefix, value);
            if (!append_ip(command, ip)) return 0;
        }
        section = plus == NULL ? NULL : plus + 1;
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
    if (strlen(message) >= sizeof(buffer)) {
        command->type = SMS_ROUTE_COMMAND_INVALID;
        command->ip_count = 0;
        return command->type;
    }
    memset(command, 0, sizeof(*command));
    snprintf(buffer, sizeof(buffer), "%s", message);
    cursor = trim_space(buffer);
    if (!take_token(&cursor, verb, sizeof(verb))) {
        return SMS_ROUTE_COMMAND_NONE;
    }

    if (text_equal_ignore_case(verb, "_smsRoute")) {
        char *nickname = trim_space(cursor);
        if (*nickname == '\0' ||
            strlen(nickname) >= sizeof(command->nickname)) {
            command->type = SMS_ROUTE_COMMAND_INVALID;
            return command->type;
        }
        snprintf(command->nickname, sizeof(command->nickname), "%s", nickname);
        command->type = SMS_ROUTE_COMMAND_ROUTE;
        return command->type;
    }

    if (!text_equal_ignore_case(verb, "_smsRegister")) {
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
