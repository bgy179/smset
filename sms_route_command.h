#ifndef SMSET_SMS_ROUTE_COMMAND_H
#define SMSET_SMS_ROUTE_COMMAND_H

#include <stddef.h>

#define SMS_ROUTE_NICKNAME_MAX 256
#define SMS_ROUTE_IP_MAX 128
#define SMS_ROUTE_IP_TEXT_MAX 16

enum sms_route_command_type {
    SMS_ROUTE_COMMAND_NONE = 0,
    SMS_ROUTE_COMMAND_ROUTE,
    SMS_ROUTE_COMMAND_REGISTER,
    SMS_ROUTE_COMMAND_INVALID
};

struct sms_route_command {
    enum sms_route_command_type type;
    char nickname[SMS_ROUTE_NICKNAME_MAX];
    char ips[SMS_ROUTE_IP_MAX][SMS_ROUTE_IP_TEXT_MAX];
    size_t ip_count;
};

enum sms_route_command_type sms_route_command_parse(
    const char *message,
    struct sms_route_command *command);

int sms_route_sender_allowed(const char *sender, const char *allowed_senders);

#endif
