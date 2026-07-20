#include "sms_command.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct test_state { int lookup_calls; int contact_inserts; int route_inserts; };

static int test_lookup(void *context, const char *key, struct sms_wechat_contact *contact) {
    struct test_state *state = context;
    assert(strcmp(key, "wxid_alice") == 0);
    ++state->lookup_calls;
    strcpy(contact->wechat_id, "wxid_alice");
    strcpy(contact->display_name, "Alice");
    strcpy(contact->phone_number, "+15551234567");
    return 0;
}

static int test_insert_contact(void *context, const struct sms_wechat_contact *contact,
                               const char *key, const char *sender) {
    struct test_state *state = context;
    assert(strcmp(contact->display_name, "Alice") == 0);
    assert(strcmp(key, "wxid_alice") == 0 && strcmp(sender, "+8613800138000") == 0);
    ++state->contact_inserts;
    return 0;
}

static int test_insert_route(void *context, const char *route, const char *sender) {
    struct test_state *state = context;
    assert(strcmp(route, "east-hub") == 0 && strcmp(sender, "+8613800138000") == 0);
    ++state->route_inserts;
    return 0;
}

int main(void) {
    struct test_state state = {0};
    struct sms_command_config config = {
        "+8613800138000", test_lookup, test_insert_contact, test_insert_route, &state
    };
    enum sms_command_type type;

    assert(sms_command_process(&config, "+8613800138000", "_smsReg wxid_alice", &type) == SMS_COMMAND_OK);
    assert(type == SMS_COMMAND_REGISTRATION && state.lookup_calls == 1 && state.contact_inserts == 1);
    assert(sms_command_process(&config, "+8613800138000", "_smsRoute east-hub", &type) == SMS_COMMAND_OK);
    assert(type == SMS_COMMAND_ROUTE && state.route_inserts == 1);
    assert(sms_command_process(&config, "+8613900138000", "_smsRoute east-hub", NULL) == SMS_COMMAND_UNAUTHORIZED_SENDER);
    assert(sms_command_process(&config, "+8613800138000", "hello", NULL) == SMS_COMMAND_NOT_A_COMMAND);
    assert(sms_command_process(&config, "+8613800138000", "_smsReg", NULL) == SMS_COMMAND_INVALID_PAYLOAD);

    puts("SMS command processing tests passed.");
    return 0;
}
