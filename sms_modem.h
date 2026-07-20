#ifndef SMSET_SMS_MODEM_H
#define SMSET_SMS_MODEM_H

#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE sms_modem_port_t;
#define SMS_MODEM_INVALID_PORT INVALID_HANDLE_VALUE
#else
typedef int sms_modem_port_t;
#define SMS_MODEM_INVALID_PORT (-1)
#endif

/* Maximum bytes for a modem AT response buffer. */
#define SMS_MODEM_RESPONSE_MAX 4096

enum sms_modem_result {
    SMS_MODEM_OK              =  0,
    SMS_MODEM_INVALID_INPUT   = -1,  /* NULL or malformed argument          */
    SMS_MODEM_PORT_ERROR      = -2,  /* failed to open or configure port    */
    SMS_MODEM_WRITE_ERROR     = -3,  /* serial write failed                 */
    SMS_MODEM_READ_TIMEOUT    = -4,  /* no response within timeout          */
    SMS_MODEM_MODEM_ERROR     = -5,  /* modem returned ERROR                */
    SMS_MODEM_NO_PROMPT       = -6,  /* modem did not return '>' prompt     */
    SMS_MODEM_NO_CONFIRM      = -7,  /* modem did not confirm +CMGS         */
    SMS_MODEM_INIT_FAILED     = -8   /* one of the init AT steps failed     */
};

/*
 * Opens and configures a serial port.
 * `port`  - device name, e.g. "COM3" on Windows or "/dev/ttyUSB0" on Linux.
 * `baud`  - baud rate, e.g. 9600 or 115200.
 * Returns SMS_MODEM_OK and writes the port handle to `*out_port` on success.
 */
int sms_modem_open(const char *port, unsigned long baud,
                   sms_modem_port_t *out_port);

/*
 * Closes a port previously opened with sms_modem_open.
 */
void sms_modem_close(sms_modem_port_t port);

/*
 * Initialises the modem for SMS text mode:
 *   1. AT         – connectivity check
 *   2. AT+CMGF=1  – enable text mode
 *   3. AT+CNMI=2,1,0,0,0 – enable incoming SMS notifications
 * Returns SMS_MODEM_OK on success.
 */
int sms_modem_init(sms_modem_port_t port);

/*
 * Sends an SMS message.
 * `phone`        - destination number; must not contain CR, LF, or '"'.
 * `message`      - UTF-8 text; must not contain the SUB character (0x1A).
 * `response`     - buffer for the final modem response (+CMGS: ... OK).
 * `response_size`- size of `response` buffer; SMS_MODEM_RESPONSE_MAX recommended.
 * Returns SMS_MODEM_OK on success.
 */
int sms_modem_send(sms_modem_port_t port, const char *phone,
                   const char *message, char *response, size_t response_size);

/*
 * Reads all messages stored in the modem (AT+CMGL="ALL").
 * `response`     - buffer for the raw modem listing.
 * `response_size`- size of `response` buffer; SMS_MODEM_RESPONSE_MAX recommended.
 * Returns SMS_MODEM_OK on success.
 */
int sms_modem_receive(sms_modem_port_t port, char *response,
                      size_t response_size);

#endif
