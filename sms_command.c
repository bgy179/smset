#include "sms_command.h"

#include <stdio.h>
#include <string.h>

/* Define SMS_COMMAND_ENABLE_DEBUG_LOG to log command classification and dispatch. */
#ifdef SMS_COMMAND_ENABLE_DEBUG_LOG
#define COMMAND_LOG(...) fprintf(stderr, "[smset-command] " __VA_ARGS__)
#else
#define COMMAND_LOG(...) ((void)0)
#endif

int wechat_lookup_contact_http(const struct wechat_lookup_config *config,
                               const char *lookup_key,
                               struct sms_wechat_contact *contact) {
    char request_body[1024];
    char response[4096];
    char command[2048];
    FILE *pipe;
    const char *api_url;
    const char *db_name;
    int timeout_ms;

    if (config == NULL || lookup_key == NULL || contact == NULL) return -1;
    if (config->api_url == NULL || config->db_name == NULL) return -1;

    api_url = config->api_url;
    db_name = config->db_name;
    timeout_ms = (config->timeout_ms > 0) ? (int)config->timeout_ms : 2000;

    snprintf(request_body, sizeof(request_body),
             "{\"dbName\":\"%s\",\"sql\":\"select * from Contact where UserName='%s'\"}",
             db_name, lookup_key);

    snprintf(command, sizeof(command),
             "curl -sS --max-time %d -X POST -H 'Content-Type: application/json' "
             "--data '%s' '%s'",
             timeout_ms, request_body, api_url);

    pipe = popen(command, "r");
    if (pipe == NULL) return -1;

    memset(response, 0, sizeof(response));
    if (fread(response, 1, sizeof(response) - 1, pipe) == 0) {
        pclose(pipe);
        return -1;
    }

    if (pclose(pipe) != 0) return -1;

    memset(contact, 0, sizeof(*contact));
    if (json_extract_string(response, "UserName", contact->wechat_id, sizeof(contact->wechat_id)) != 0) {
        return -1;
    }
    if (json_extract_string(response, "NickName", contact->display_name, sizeof(contact->display_name)) != 0) {
        return -1;
    }
    if (json_extract_string(response, "Phone", contact->phone_number, sizeof(contact->phone_number)) != 0) {
        return -1;
    }
    if (contact->wechat_id[0] == '\0' || contact->display_name[0] == '\0') {
        return -1;
    }
    return 0;
}

/* Returns the nonempty, whitespace-trimmed payload after a recognized prefix. */
static int command_payload(const char *message, const char *prefix,
                           char payload[SMS_COMMAND_PAYLOAD_MAX]) {
    size_t prefix_length = strlen(prefix);
    size_t length;

    if (strncmp(message, prefix, prefix_length) != 0) return SMS_COMMAND_NOT_A_COMMAND;
    message += prefix_length;
    while (*message == ' ' || *message == '\t') ++message;
    length = strlen(message);
    while (length > 0 && (message[length - 1] == ' ' || message[length - 1] == '\t' ||
                          message[length - 1] == '\r' || message[length - 1] == '\n')) --length;
    if (length == 0 || length >= SMS_COMMAND_PAYLOAD_MAX) return SMS_COMMAND_INVALID_PAYLOAD;
    memcpy(payload, message, length);
    payload[length] = '\0';
    return SMS_COMMAND_OK;
}

/* Authorizes, recognizes, and dispatches an incoming command SMS. */
int sms_command_process(const struct sms_command_config *config,
                        const char *sender_phone, const char *message,
                        enum sms_command_type *processed_type) {
    char payload[SMS_COMMAND_PAYLOAD_MAX];
    int result;

    if (processed_type != NULL) *processed_type = SMS_COMMAND_REGISTRATION;
    if (config == NULL || config->authorized_phone == NULL || sender_phone == NULL || message == NULL)
        return SMS_COMMAND_INVALID_INPUT;
    if (strcmp(config->authorized_phone, sender_phone) != 0) {
        COMMAND_LOG("Ignored SMS from unauthorized sender %s\n", sender_phone);
        return SMS_COMMAND_UNAUTHORIZED_SENDER;
    }
    COMMAND_LOG("Authorized SMS received from %s; checking command prefix\n", sender_phone);

    result = command_payload(message, "_smsReg", payload);
    if (result == SMS_COMMAND_OK) {
        struct sms_wechat_contact contact;
        if (config->lookup_wechat_contact == NULL || config->insert_contact == NULL)
            return SMS_COMMAND_INVALID_INPUT;
        COMMAND_LOG("Processing _smsReg with lookup key length %zu\n", strlen(payload));
        memset(&contact, 0, sizeof(contact));
        if (config->lookup_wechat_contact(config->context, payload, &contact) != 0) {
            COMMAND_LOG("WeChat contact lookup failed\n");
            return SMS_COMMAND_WECHAT_FAILED;
        }
        if (config->insert_contact(config->context, &contact, payload, sender_phone) != 0) {
            COMMAND_LOG("MySQL contact insert failed\n");
            return SMS_COMMAND_DATABASE_FAILED;
        }
        if (processed_type != NULL) *processed_type = SMS_COMMAND_REGISTRATION;
        COMMAND_LOG("_smsReg completed successfully\n");
        return SMS_COMMAND_OK;
    }
    if (result == SMS_COMMAND_INVALID_PAYLOAD) return result;

    result = command_payload(message, "_smsRoute", payload);
    if (result == SMS_COMMAND_OK) {
        if (config->insert_route == NULL) return SMS_COMMAND_INVALID_INPUT;
        COMMAND_LOG("Processing _smsRoute with payload length %zu\n", strlen(payload));
        if (config->insert_route(config->context, payload, sender_phone) != 0) {
            COMMAND_LOG("MySQL route insert failed\n");
            return SMS_COMMAND_DATABASE_FAILED;
        }
        if (processed_type != NULL) *processed_type = SMS_COMMAND_ROUTE;
        COMMAND_LOG("_smsRoute completed successfully\n");
        return SMS_COMMAND_OK;
    }
    if (result == SMS_COMMAND_INVALID_PAYLOAD) return result;
    COMMAND_LOG("Authorized SMS is not a command\n");
    return SMS_COMMAND_NOT_A_COMMAND;
}
