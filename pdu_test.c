#include "pdu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *text = "Hello, 中国! 😀";
    char pdu[128];
    char decoded[128];
    size_t pdu_length;
    size_t decoded_length;
    size_t data_units;
    enum sms_pdu_encoding encoding;

    assert(sms_pdu_encode_utf8(text, pdu, sizeof(pdu), &pdu_length) == SMS_PDU_OK);
    assert(strcmp(pdu, "00480065006C006C006F002C00204E2D56FD00210020D83DDE00") == 0);
    assert(pdu_length == strlen(pdu));
    assert(sms_pdu_decode_utf8(pdu, decoded, sizeof(decoded), &decoded_length) == SMS_PDU_OK);
    assert(strcmp(decoded, text) == 0);
    assert(decoded_length == strlen(text));
    assert(sms_pdu_decode_utf8("D800", decoded, sizeof(decoded), NULL) == SMS_PDU_INVALID_UTF16);
    assert(sms_pdu_encode_utf8("\xC0\x80", pdu, sizeof(pdu), NULL) == SMS_PDU_INVALID_UTF8);

    assert(sms_pdu_encode_text(SMS_PDU_GSM7, "hello^", pdu, sizeof(pdu), &data_units) == SMS_PDU_OK);
    assert(data_units == 7); /* '^' occupies the GSM7 escape plus one septet. */
    assert(sms_pdu_decode_text(SMS_PDU_GSM7, pdu, data_units, decoded, sizeof(decoded), NULL) == SMS_PDU_OK);
    assert(strcmp(decoded, "hello^") == 0);

    assert(sms_pdu_encode_text(SMS_PDU_8BIT, "中国", pdu, sizeof(pdu), &data_units) == SMS_PDU_OK);
    assert(data_units == strlen("中国"));
    assert(sms_pdu_decode_text(SMS_PDU_8BIT, pdu, data_units, decoded, sizeof(decoded), NULL) == SMS_PDU_OK);
    assert(strcmp(decoded, "中国") == 0);

    assert(sms_pdu_encode_text(SMS_PDU_GSM7, "中国", pdu, sizeof(pdu), NULL) == SMS_PDU_UNSUPPORTED_CHAR);
    assert(sms_pdu_detect_encoding("Plain ^ text", &encoding) == SMS_PDU_OK);
    assert(encoding == SMS_PDU_GSM7);
    assert(sms_pdu_detect_encoding("中国", &encoding) == SMS_PDU_OK);
    assert(encoding == SMS_PDU_UCS2);
    assert(sms_pdu_detect_encoding("\xC0\x80", &encoding) == SMS_PDU_INVALID_UTF8);
    assert(sms_pdu_encode_auto("Auto ^", pdu, sizeof(pdu), &encoding, &data_units) == SMS_PDU_OK);
    assert(encoding == SMS_PDU_GSM7 && data_units == 7);
    assert(sms_pdu_decode_text(encoding, pdu, data_units, decoded, sizeof(decoded), NULL) == SMS_PDU_OK);
    assert(strcmp(decoded, "Auto ^") == 0);
    assert(sms_pdu_encode_auto("自动编码", pdu, sizeof(pdu), &encoding, &data_units) == SMS_PDU_OK);
    assert(encoding == SMS_PDU_UCS2 && data_units == 8);
    assert(strcmp(pdu, "81EA52A87F167801") == 0);

    printf("PDU encoding tests passed (GSM7, 8BIT UTF-8, and UCS2).\n");
    return 0;
}
