#ifndef SMSET_PDU_H
#define SMSET_PDU_H

#include <stddef.h>

/*
 * Converts between UTF-8 text and the hexadecimal UCS-2/UTF-16BE SMS user
 * data used by text-mode PDU messages. The output is not a complete SMS TPDU:
 * it is the encoded message body suitable for a PDU's user-data field.
 *
 * The encoder emits UTF-16BE code units. Characters above U+FFFF are encoded
 * as a surrogate pair, so all valid UTF-8 text, including Chinese characters,
 * is supported.
 */
enum sms_pdu_result {
    SMS_PDU_OK = 0,
    SMS_PDU_INVALID_UTF8 = -1,
    SMS_PDU_INVALID_HEX = -2,
    SMS_PDU_INVALID_UTF16 = -3,
    SMS_PDU_NO_SPACE = -4,
    SMS_PDU_UNSUPPORTED_CHAR = -5
};

enum sms_pdu_encoding {
    SMS_PDU_GSM7,
    SMS_PDU_8BIT,
    SMS_PDU_UCS2
};

/*
 * Selects GSM7 when every UTF-8 character belongs to GSM 03.38 (including its
 * extension table); otherwise selects UCS2. Invalid UTF-8 is reported as an
 * error. Automatic selection never chooses 8BIT because it is not reliably
 * displayed as text by all SMS recipients.
 */
int sms_pdu_detect_encoding(const char *utf8, enum sms_pdu_encoding *encoding);

/*
 * Detects GSM7 versus UCS2 and encodes the UTF-8 text in one call. `encoding`
 * is optional and receives the selected encoding when supplied. `data_units`
 * is optional and receives the GSM7 septet count or UCS2 byte count.
 */
int sms_pdu_encode_auto(const char *utf8, char *hex_output,
                        size_t hex_output_size, enum sms_pdu_encoding *encoding,
                        size_t *data_units);

/*
 * Encodes UTF-8 text as packed GSM 7-bit, raw 8-bit UTF-8 bytes, or UCS-2.
 * `data_units` receives the septet count for GSM7 and byte count otherwise.
 * GSM7 only accepts characters in the GSM 03.38 default alphabet; use UCS2
 * (or 8BIT for UTF-8 byte transport) for Chinese and other Unicode text.
 */
int sms_pdu_encode_text(enum sms_pdu_encoding encoding, const char *utf8,
                        char *hex_output, size_t hex_output_size,
                        size_t *data_units);

/*
 * Decodes a hexadecimal user-data field. For GSM7, `data_units` must be the
 * TP-UDL septet count; for 8BIT/UCS2 it is the byte count (pass 0 to infer it
 * from the hexadecimal input length).
 */
int sms_pdu_decode_text(enum sms_pdu_encoding encoding, const char *hex_input,
                        size_t data_units, char *utf8_output,
                        size_t utf8_output_size, size_t *utf8_length);

/* Encodes UTF-8 text as hexadecimal UCS-2/UTF-16BE SMS user data. */
int sms_pdu_encode_utf8(const char *utf8, char *hex_output,
                        size_t hex_output_size, size_t *hex_length);

/* Decodes hexadecimal UCS-2/UTF-16BE SMS user data into a UTF-8 string. */
int sms_pdu_decode_utf8(const char *hex_input, char *utf8_output,
                        size_t utf8_output_size, size_t *utf8_length);

#endif
