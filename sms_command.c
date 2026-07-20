#include "sms_command.h"
#include "jsmn.h"
#include <mysql.h>
#include <stdio.h>
#include <string.h>

MYSQL *mysql_conn;

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

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
    unsigned int timeout_ms;

    if (config == NULL || lookup_key == NULL || contact == NULL) return -1;
    if (config->api_url == NULL || config->db_name == NULL) return -1;

    timeout_ms = (config->timeout_ms > 0u) ? config->timeout_ms : 2000u;
    snprintf(request_body, sizeof(request_body),
             "{\"dbName\":\"%s\",\"sql\":\"select NickName, UserName from Contact where NickName='%s'\"}",
             config->db_name, lookup_key);

    snprintf(command, sizeof(command),
             "curl -sS --max-time %u -X POST -H 'Content-Type: application/json' "
             "--data '%s' '%s'",
             timeout_ms, request_body, config->api_url);

    pipe = popen(command, "r");
    if (pipe == NULL) return -1;

    memset(response, 0, sizeof(response));
    if (fread(response, 1, sizeof(response) - 1, pipe) == 0) {
        pclose(pipe);
        return -1;
    }

    if (pclose(pipe) != 0) return -1;

    memset(contact, 0, sizeof(*contact));

    jsmn_parser p;
    jsmn_init(&p);
    jsmntok_t tokens[128];
    int r = jsmn_parse(&p, response, strlen(response), tokens, sizeof(tokens) / sizeof(tokens[0]));

    if (r < 0) {
        return -1;
    }
    if (r < 1 || tokens[0].type != JSMN_OBJECT) {
        return -1;
    }

    for (int i = 1; i < r; i++) {
        jsmntok_t *key = &tokens[i];
        if (key->type != JSMN_STRING) continue;
        if (i + 1 >= r) break;
        jsmntok_t *value = &tokens[i + 1];

        if (jsoneq(response, key, "UserName") == 0) {
            size_t len = value->end - value->start;
            if (len >= sizeof(contact->wechat_id)) return -1;
            memcpy(contact->wechat_id, response + value->start, len);
            contact->wechat_id[len] = '\0';
        } else if (jsoneq(response, key, "NickName") == 0) {
            size_t len = value->end - value->start;
            if (len >= sizeof(contact->nick_name)) return -1;
            memcpy(contact->nick_name, response + value->start, len);
            contact->nick_name[len] = '\0';
        }
        i++;
    }

    if (contact->wechat_id[0] == '\0' || contact->nick_name[0] == '\0') {
        return -1;
    }
    return 0;
}

int insert_chatroom(void *context, const struct sms_wechat_contact *contact,
                    const char *tenantname)
{
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "INSERT INTO wzwmonitor.wechat_chatroom(wxid, nickname, tenantname) "
                "VALUES('%s', '%s', '%s');", contact->wechat_id, contact->nick_name, tenantname);

    mysql_query(g_mysql_connection, sql);
    
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
