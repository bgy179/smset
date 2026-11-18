#include "sms_route_command.h"

#include <assert.h>
#include <string.h>

int main(void) {
    struct sms_route_command command;

    assert(sms_route_command_parse(
        "  _SMSROUTE  告警群 A  ", &command) == SMS_ROUTE_COMMAND_ROUTE);
    assert(strcmp(command.nickname, "告警群 A") == 0);

    assert(sms_route_command_parse(
        "_sMsReGiStEr group 10.1.2.3,10.1.2.5-7+9+6", &command) ==
        SMS_ROUTE_COMMAND_REGISTER);
    assert(command.ip_count == 5);
    assert(strcmp(command.ips[0], "10.1.2.3") == 0);
    assert(strcmp(command.ips[3], "10.1.2.7") == 0);
    assert(strcmp(command.ips[4], "10.1.2.9") == 0);

    assert(sms_route_command_parse(
        "_smsRegister group 10.1.2.1+3-5+9", &command) ==
        SMS_ROUTE_COMMAND_REGISTER);
    assert(command.ip_count == 5);
    assert(strcmp(command.ips[0], "10.1.2.1") == 0);
    assert(strcmp(command.ips[2], "10.1.2.4") == 0);
    assert(strcmp(command.ips[4], "10.1.2.9") == 0);

    assert(sms_route_command_parse(
        " \t_smsREGISTER\t\"告警 群\"\t10.1.2.1+3-5;10.1.2.1+9 \t",
        &command) == SMS_ROUTE_COMMAND_REGISTER);
    assert(strcmp(command.nickname, "告警 群") == 0);
    assert(command.ip_count == 5);

    assert(sms_route_command_parse(
        "_smsRegister group 10.1.2.999", &command) ==
        SMS_ROUTE_COMMAND_INVALID);
    assert(sms_route_command_parse(
        "_smsRegister group 10.1.2.8-3", &command) ==
        SMS_ROUTE_COMMAND_INVALID);
    assert(sms_route_command_parse(
        "_smsRegister group 10.1.2.1+", &command) ==
        SMS_ROUTE_COMMAND_INVALID);
    assert(sms_route_command_parse(
        "_smsRegister group 10.1.2.3-5-7", &command) ==
        SMS_ROUTE_COMMAND_INVALID);
    assert(sms_route_command_parse(
        "_smsRoute   ", &command) == SMS_ROUTE_COMMAND_INVALID);
    assert(sms_route_command_parse(
        "_smsRoute group extra", &command) == SMS_ROUTE_COMMAND_ROUTE);
    assert(sms_route_command_parse(
        "ordinary alert", &command) == SMS_ROUTE_COMMAND_NONE);

    assert(sms_route_sender_allowed("+8613800138000",
                                    "+8613800138000, +8613900139000"));
    assert(!sms_route_sender_allowed("+8613700137000",
                                     "+8613800138000,+8613900139000"));
    return 0;
}
