#ifndef SMSET_SMS_COMMAND_H
#define SMSET_SMS_COMMAND_H

/* Maximum bytes accepted after a command prefix, excluding its terminator. */
#define SMS_COMMAND_PAYLOAD_MAX 512

enum sms_command_result {
    SMS_COMMAND_OK = 0,
    SMS_COMMAND_NOT_A_COMMAND = 1,
    SMS_COMMAND_UNAUTHORIZED_SENDER = 2,
    SMS_COMMAND_INVALID_INPUT = -1,
    SMS_COMMAND_INVALID_PAYLOAD = -2,
    SMS_COMMAND_WECHAT_FAILED = -3,
    SMS_COMMAND_DATABASE_FAILED = -4
};

enum sms_command_type {
    SMS_COMMAND_REGISTRATION,
    SMS_COMMAND_ROUTE
};

/* Normalized contact information obtained from the configured WeChat API. */
struct sms_wechat_contact {
    char wechat_id[128];
    char display_name[256];
    char phone_number[64];
};

/*
 * Implement this adapter with the appropriate WeChat API for the deployment.
 * `lookup_key` is the text following `_smsReg`; return 0 for success.
 */
typedef int (*sms_wechat_lookup_fn)(void *context, const char *lookup_key,
                                    struct sms_wechat_contact *contact);

/*
 * Implement these adapters with prepared MySQL statements. They must not build
 * SQL by string concatenation with command data. Return 0 for success.
 */
typedef int (*sms_mysql_insert_contact_fn)(void *context,
                                           const struct sms_wechat_contact *contact,
                                           const char *lookup_key,
                                           const char *sender_phone);
typedef int (*sms_mysql_insert_route_fn)(void *context, const char *route_data,
                                         const char *sender_phone);

struct sms_command_config {
    const char *authorized_phone;
    sms_wechat_lookup_fn lookup_wechat_contact;
    sms_mysql_insert_contact_fn insert_contact;
    sms_mysql_insert_route_fn insert_route;
    void *context;
};

struct wechat_lookup_config {
    const char *api_url;
    const char *db_name;
    unsigned int timeout_ms;
};

int wechat_lookup_contact_http(const struct wechat_lookup_config *config,
                               const char *lookup_key,
                               struct sms_wechat_contact *contact);

/*
 * Checks that sender_phone exactly matches `authorized_phone`, recognizes an
 * SMS beginning with `_smsReg` or `_smsRoute`, and dispatches it. `_smsReg`
 * queries WeChat then stores the returned contact; `_smsRoute` stores its
 * payload directly. Noncommands and unauthorized messages have nonnegative
 * return values and perform no external operation.
 */
int sms_command_process(const struct sms_command_config *config,
                        const char *sender_phone, const char *message,
                        enum sms_command_type *processed_type);

#endif
