#include "pdu.h"

#include <stdint.h>
#include <string.h>

/* Define SMS_PDU_ENABLE_DEBUG_LOG at compile time to trace codec operations. */
#ifdef SMS_PDU_ENABLE_DEBUG_LOG
#include <stdio.h>
#define PDU_LOG(...) fprintf(stderr, "[smset-pdu] " __VA_ARGS__)
#else
#define PDU_LOG(...) ((void)0)
#endif

/* Reads one strictly-valid UTF-8 sequence and advances the input pointer. */
static int utf8_next(const unsigned char **input, uint32_t *code_point) {
    const unsigned char *s = *input;
    uint32_t cp;

    if (s[0] < 0x80) {
        *code_point = s[0];
        *input = s + 1;
        return SMS_PDU_OK;
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *input = s + 2;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && !(s[0] == 0xE0 && s[1] < 0xA0) &&
               !(s[0] == 0xED && s[1] >= 0xA0)) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        *input = s + 3;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80 &&
               !(s[0] == 0xF0 && s[1] < 0x90) &&
               !(s[0] == 0xF4 && s[1] > 0x8F)) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) |
             ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        *input = s + 4;
    } else {
        return SMS_PDU_INVALID_UTF8;
    }
    *code_point = cp;
    return SMS_PDU_OK;
}

/* Converts one hexadecimal digit to its numeric value. */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Appends one binary byte as two uppercase hexadecimal characters. */
static int append_hex_byte(unsigned char value, char *output, size_t output_size,
                           size_t *index) {
    static const char digits[] = "0123456789ABCDEF";
    if (*index > output_size || output_size - *index < 2) return SMS_PDU_NO_SPACE;
    output[(*index)++] = digits[value >> 4];
    output[(*index)++] = digits[value & 0x0F];
    return SMS_PDU_OK;
}

/* Reads one binary byte from a hexadecimal string at a byte-oriented offset. */
static int hex_byte_at(const char *input, size_t byte_index, unsigned char *value) {
    int high = hex_value(input[byte_index * 2]);
    int low = hex_value(input[byte_index * 2 + 1]);
    if (high < 0 || low < 0) return SMS_PDU_INVALID_HEX;
    *value = (unsigned char)((high << 4) | low);
    return SMS_PDU_OK;
}

/* Maps one Unicode code point to GSM 03.38, including extension-table escapes. */
static int gsm7_from_code_point(uint32_t cp, unsigned char *value, int *extension) {
    static const uint32_t basic[] = {
        '@', 0x00A3, '$', 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
        0x00F2, 0x00C7, '\n', 0x00D8, 0x00F8, '\r', 0x00C5, 0x00E5,
        0x0394, '_', 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
        0x03A3, 0x0398, 0x039E, 0x1B, 0x00C6, 0x00E6, 0x00DF, 0x00C9
    };
    static const uint32_t extension_chars[] = {'\f', '^', '{', '}', '\\', '[', '~', ']', '|', 0x20AC};
    static const unsigned char extension_values[] = {0x0A, 0x14, 0x28, 0x29, 0x2F, 0x3C, 0x3D, 0x3E, 0x40, 0x65};
    size_t i;

    if (cp == '@') { *value = 0; *extension = 0; return SMS_PDU_OK; }
    for (i = 0; i < sizeof(extension_chars) / sizeof(extension_chars[0]); ++i) {
        if (extension_chars[i] == cp) { *value = extension_values[i]; *extension = 1; return SMS_PDU_OK; }
    }
    for (i = 0; i < sizeof(basic) / sizeof(basic[0]); ++i) {
        if (basic[i] == cp) { *value = (unsigned char)i; *extension = 0; return SMS_PDU_OK; }
    }
    if (cp >= 0x20 && cp <= 0x7A) { *value = (unsigned char)cp; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00A4) { *value = 0x24; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00A1) { *value = 0x40; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00BF) { *value = 0x60; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00C4) { *value = 0x5B; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00D6) { *value = 0x5C; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00D1) { *value = 0x5D; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00DC) { *value = 0x5E; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00A7) { *value = 0x5F; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00E4) { *value = 0x7B; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00F6) { *value = 0x7C; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00F1) { *value = 0x7D; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00FC) { *value = 0x7E; *extension = 0; return SMS_PDU_OK; }
    if (cp == 0x00E0) { *value = 0x7F; *extension = 0; return SMS_PDU_OK; }
    return SMS_PDU_UNSUPPORTED_CHAR;
}

/* Maps one GSM 03.38 septet (optionally extension-table) to Unicode. */
static int gsm7_to_code_point(unsigned char value, int extension, uint32_t *cp) {
    static const uint32_t basic[] = {
        '@', 0x00A3, '$', 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
        0x00F2, 0x00C7, '\n', 0x00D8, 0x00F8, '\r', 0x00C5, 0x00E5,
        0x0394, '_', 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
        0x03A3, 0x0398, 0x039E, 0x1B, 0x00C6, 0x00E6, 0x00DF, 0x00C9
    };
    if (extension) {
        switch (value) {
            case 0x0A: *cp = '\f'; return SMS_PDU_OK; case 0x14: *cp = '^'; return SMS_PDU_OK;
            case 0x28: *cp = '{'; return SMS_PDU_OK;
            case 0x29: *cp = '}'; return SMS_PDU_OK; case 0x2F: *cp = '\\'; return SMS_PDU_OK;
            case 0x3C: *cp = '['; return SMS_PDU_OK; case 0x3D: *cp = '~'; return SMS_PDU_OK;
            case 0x3E: *cp = ']'; return SMS_PDU_OK; case 0x40: *cp = '|'; return SMS_PDU_OK;
            case 0x65: *cp = 0x20AC; return SMS_PDU_OK; default: return SMS_PDU_UNSUPPORTED_CHAR;
        }
    }
    if (value < 0x20) { *cp = basic[value]; return SMS_PDU_OK; }
    if (value >= 0x20 && value <= 0x7A) { *cp = value; return SMS_PDU_OK; }
    switch (value) {
        case 0x24: *cp = 0x00A4; break; case 0x40: *cp = 0x00A1; break;
        case 0x5B: *cp = 0x00C4; break; case 0x5C: *cp = 0x00D6; break;
        case 0x5D: *cp = 0x00D1; break; case 0x5E: *cp = 0x00DC; break;
        case 0x5F: *cp = 0x00A7; break; case 0x60: *cp = 0x00BF; break;
        case 0x7B: *cp = 0x00E4; break; case 0x7C: *cp = 0x00F6; break;
        case 0x7D: *cp = 0x00F1; break; case 0x7E: *cp = 0x00FC; break;
        case 0x7F: *cp = 0x00E0; break; default: return SMS_PDU_UNSUPPORTED_CHAR;
    }
    return SMS_PDU_OK;
}

/* Chooses GSM7 for GSM 03.38 text and UCS2 for any other valid Unicode text. */
int sms_pdu_detect_encoding(const char *utf8, enum sms_pdu_encoding *encoding) {
    const unsigned char *input = (const unsigned char *)utf8;

    if (utf8 == NULL || encoding == NULL) return SMS_PDU_NO_SPACE;
    PDU_LOG("Encoding detection: inspecting %zu UTF-8 byte(s)\n", strlen(utf8));
    while (*input != '\0') {
        uint32_t cp;
        unsigned char septet;
        int extension;
        int result = utf8_next(&input, &cp);
        if (result != SMS_PDU_OK) {
            PDU_LOG("Encoding detection: invalid UTF-8 input\n");
            return result;
        }
        result = gsm7_from_code_point(cp, &septet, &extension);
        if (result != SMS_PDU_OK) {
            *encoding = SMS_PDU_UCS2;
            PDU_LOG("Encoding detection: U+%04lX requires UCS2\n", (unsigned long)cp);
            return SMS_PDU_OK;
        }
        PDU_LOG("Encoding detection: U+%04lX fits GSM7%s\n", (unsigned long)cp,
                extension ? " extension table" : "");
    }
    *encoding = SMS_PDU_GSM7;
    PDU_LOG("Encoding detection: selected GSM7\n");
    return SMS_PDU_OK;
}

/* Detects a suitable text encoding and writes its hexadecimal PDU user data. */
int sms_pdu_encode_auto(const char *utf8, char *hex_output,
                        size_t hex_output_size, enum sms_pdu_encoding *encoding,
                        size_t *data_units) {
    enum sms_pdu_encoding detected;
    int result = sms_pdu_detect_encoding(utf8, &detected);

    if (result != SMS_PDU_OK) return result;
    PDU_LOG("Automatic encode: selected %s\n", detected == SMS_PDU_GSM7 ? "GSM7" : "UCS2");
    result = sms_pdu_encode_text(detected, utf8, hex_output, hex_output_size, data_units);
    if (result == SMS_PDU_OK && encoding != NULL) *encoding = detected;
    return result;
}

/* Appends a UTF-16 code unit as four uppercase hexadecimal characters. */
static int append_hex_unit(uint16_t unit, char *output, size_t output_size,
                           size_t *index) {
    static const char digits[] = "0123456789ABCDEF";
    if (*index > output_size || output_size - *index < 4) return SMS_PDU_NO_SPACE;
    output[(*index)++] = digits[(unit >> 12) & 0x0F];
    output[(*index)++] = digits[(unit >> 8) & 0x0F];
    output[(*index)++] = digits[(unit >> 4) & 0x0F];
    output[(*index)++] = digits[unit & 0x0F];
    return SMS_PDU_OK;
}

/* Appends one Unicode code point as its canonical UTF-8 byte sequence. */
static int append_utf8(uint32_t cp, char *output, size_t output_size,
                       size_t *index) {
    size_t count = cp <= 0x7F ? 1 : cp <= 0x7FF ? 2 : cp <= 0xFFFF ? 3 : 4;
    if (*index > output_size || output_size - *index < count) return SMS_PDU_NO_SPACE;
    if (count == 1) {
        output[(*index)++] = (char)cp;
    } else if (count == 2) {
        output[(*index)++] = (char)(0xC0 | (cp >> 6));
        output[(*index)++] = (char)(0x80 | (cp & 0x3F));
    } else if (count == 3) {
        output[(*index)++] = (char)(0xE0 | (cp >> 12));
        output[(*index)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        output[(*index)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        output[(*index)++] = (char)(0xF0 | (cp >> 18));
        output[(*index)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        output[(*index)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        output[(*index)++] = (char)(0x80 | (cp & 0x3F));
    }
    return SMS_PDU_OK;
}

/* Encodes UTF-8 input into UCS-2/UTF-16BE user data represented as hex. */
int sms_pdu_encode_utf8(const char *utf8, char *hex_output,
                        size_t hex_output_size, size_t *hex_length) {
    const unsigned char *input = (const unsigned char *)utf8;
    size_t index = 0;
    int result;

    if (utf8 == NULL || hex_output == NULL || hex_output_size == 0) return SMS_PDU_NO_SPACE;
    PDU_LOG("UCS2 encode: input bytes=%zu, output capacity=%zu\n", strlen(utf8), hex_output_size);
    while (*input != '\0') {
        uint32_t cp;
        result = utf8_next(&input, &cp);
        if (result != SMS_PDU_OK) return result;
        PDU_LOG("UCS2 encode: code point U+%04lX\n", (unsigned long)cp);
        if (cp <= 0xFFFF) {
            result = append_hex_unit((uint16_t)cp, hex_output, hex_output_size - 1, &index);
        } else {
            uint32_t value = cp - 0x10000;
            result = append_hex_unit((uint16_t)(0xD800 | (value >> 10)), hex_output, hex_output_size - 1, &index);
            if (result == SMS_PDU_OK)
                result = append_hex_unit((uint16_t)(0xDC00 | (value & 0x3FF)), hex_output, hex_output_size - 1, &index);
        }
        if (result != SMS_PDU_OK) return result;
    }
    hex_output[index] = '\0';
    if (hex_length != NULL) *hex_length = index;
    PDU_LOG("UCS2 encode: produced %zu hex characters\n", index);
    return SMS_PDU_OK;
}

/* Decodes UCS-2/UTF-16BE hexadecimal user data into a UTF-8 string. */
int sms_pdu_decode_utf8(const char *hex_input, char *utf8_output,
                        size_t utf8_output_size, size_t *utf8_length) {
    size_t input_length, offset = 0, index = 0;

    if (hex_input == NULL || utf8_output == NULL || utf8_output_size == 0) return SMS_PDU_NO_SPACE;
    input_length = strlen(hex_input);
    PDU_LOG("UCS2 decode: input hex characters=%zu, output capacity=%zu\n", input_length, utf8_output_size);
    if (input_length % 4 != 0) return SMS_PDU_INVALID_HEX;

    while (offset < input_length) {
        int a = hex_value(hex_input[offset]);
        int b = hex_value(hex_input[offset + 1]);
        int c = hex_value(hex_input[offset + 2]);
        int d = hex_value(hex_input[offset + 3]);
        uint16_t unit;
        uint32_t cp;
        int result;
        if (a < 0 || b < 0 || c < 0 || d < 0) return SMS_PDU_INVALID_HEX;
        unit = (uint16_t)((a << 12) | (b << 8) | (c << 4) | d);
        offset += 4;
        PDU_LOG("UCS2 decode: UTF-16 unit=0x%04X\n", unit);

        if (unit >= 0xD800 && unit <= 0xDBFF) {
            int e, f, g, h;
            uint16_t low;
            if (offset + 4 > input_length) return SMS_PDU_INVALID_UTF16;
            e = hex_value(hex_input[offset]); f = hex_value(hex_input[offset + 1]);
            g = hex_value(hex_input[offset + 2]); h = hex_value(hex_input[offset + 3]);
            if (e < 0 || f < 0 || g < 0 || h < 0) return SMS_PDU_INVALID_HEX;
            low = (uint16_t)((e << 12) | (f << 8) | (g << 4) | h);
            if (low < 0xDC00 || low > 0xDFFF) return SMS_PDU_INVALID_UTF16;
            offset += 4;
            cp = 0x10000 + (((uint32_t)unit - 0xD800) << 10) + ((uint32_t)low - 0xDC00);
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return SMS_PDU_INVALID_UTF16;
        } else {
            cp = unit;
        }
        result = append_utf8(cp, utf8_output, utf8_output_size - 1, &index);
        if (result != SMS_PDU_OK) return result;
    }
    utf8_output[index] = '\0';
    if (utf8_length != NULL) *utf8_length = index;
    PDU_LOG("UCS2 decode: produced %zu UTF-8 bytes\n", index);
    return SMS_PDU_OK;
}

/* Encodes UTF-8 text using the selected PDU alphabet and packing rules. */
int sms_pdu_encode_text(enum sms_pdu_encoding encoding, const char *utf8,
                        char *hex_output, size_t hex_output_size,
                        size_t *data_units) {
    const unsigned char *input = (const unsigned char *)utf8;
    size_t index = 0, units = 0;
    uint32_t bits = 0;
    unsigned int bit_count = 0;
    int result;

    PDU_LOG("Encode request: encoding=%d\n", (int)encoding);
    if (encoding == SMS_PDU_UCS2) {
        size_t hex_length;
        result = sms_pdu_encode_utf8(utf8, hex_output, hex_output_size, &hex_length);
        if (result == SMS_PDU_OK && data_units != NULL) *data_units = hex_length / 2;
        return result;
    }
    if (utf8 == NULL || hex_output == NULL || hex_output_size == 0) return SMS_PDU_NO_SPACE;

    if (encoding == SMS_PDU_8BIT) {
        const unsigned char *check = input;
        while (*check != '\0') {
            uint32_t cp;
            result = utf8_next(&check, &cp);
            if (result != SMS_PDU_OK) return result;
            (void)cp;
        }
        while (*input != '\0') {
            result = append_hex_byte(*input++, hex_output, hex_output_size - 1, &index);
            if (result != SMS_PDU_OK) return result;
            ++units;
        }
        PDU_LOG("8BIT encode: copied %zu UTF-8 byte(s)\n", units);
    } else if (encoding == SMS_PDU_GSM7) {
        while (*input != '\0') {
            uint32_t cp;
            unsigned char septet;
            int extension;
            result = utf8_next(&input, &cp);
            if (result != SMS_PDU_OK) return result;
            result = gsm7_from_code_point(cp, &septet, &extension);
            if (result != SMS_PDU_OK) return result;
            PDU_LOG("GSM7 encode: U+%04lX -> septet 0x%02X%s\n", (unsigned long)cp,
                    septet, extension ? " (extension)" : "");
            if (extension) {
                bits |= (uint32_t)0x1B << bit_count;
                bit_count += 7;
                ++units;
                while (bit_count >= 8) {
                    result = append_hex_byte((unsigned char)bits, hex_output, hex_output_size - 1, &index);
                    if (result != SMS_PDU_OK) return result;
                    bits >>= 8;
                    bit_count -= 8;
                }
            }
            bits |= (uint32_t)septet << bit_count;
            bit_count += 7;
            ++units;
            while (bit_count >= 8) {
                result = append_hex_byte((unsigned char)bits, hex_output, hex_output_size - 1, &index);
                if (result != SMS_PDU_OK) return result;
                bits >>= 8;
                bit_count -= 8;
            }
        }
        if (bit_count != 0) {
            result = append_hex_byte((unsigned char)bits, hex_output, hex_output_size - 1, &index);
            if (result != SMS_PDU_OK) return result;
        }
        PDU_LOG("GSM7 encode: packed %zu septet(s) into %zu byte(s)\n", units, index / 2);
    } else {
        return SMS_PDU_UNSUPPORTED_CHAR;
    }
    hex_output[index] = '\0';
    if (data_units != NULL) *data_units = units;
    PDU_LOG("Encode complete: hex characters=%zu, data units=%zu\n", index, units);
    return SMS_PDU_OK;
}

/* Decodes hexadecimal user data according to the selected PDU alphabet. */
int sms_pdu_decode_text(enum sms_pdu_encoding encoding, const char *hex_input,
                        size_t data_units, char *utf8_output,
                        size_t utf8_output_size, size_t *utf8_length) {
    size_t hex_length, byte_count, offset = 0, index = 0;
    int result;

    PDU_LOG("Decode request: encoding=%d, data units=%zu\n", (int)encoding, data_units);
    if (encoding == SMS_PDU_UCS2) {
        hex_length = hex_input == NULL ? 0 : strlen(hex_input);
        if (data_units != 0 && data_units * 2 != hex_length) return SMS_PDU_INVALID_HEX;
        return sms_pdu_decode_utf8(hex_input, utf8_output, utf8_output_size, utf8_length);
    }
    if (hex_input == NULL || utf8_output == NULL || utf8_output_size == 0) return SMS_PDU_NO_SPACE;
    hex_length = strlen(hex_input);
    if (hex_length % 2 != 0) return SMS_PDU_INVALID_HEX;
    byte_count = hex_length / 2;
    while (offset < byte_count) {
        unsigned char byte;
        result = hex_byte_at(hex_input, offset++, &byte);
        if (result != SMS_PDU_OK) return result;
    }
    offset = 0;

    if (encoding == SMS_PDU_8BIT) {
        const unsigned char *check;
        if (data_units != 0 && data_units != byte_count) return SMS_PDU_INVALID_HEX;
        if (byte_count >= utf8_output_size) return SMS_PDU_NO_SPACE;
        for (offset = 0; offset < byte_count; ++offset) {
            unsigned char byte;
            (void)hex_byte_at(hex_input, offset, &byte);
            if (byte == 0) return SMS_PDU_INVALID_UTF8;
            utf8_output[offset] = (char)byte;
        }
        utf8_output[byte_count] = '\0';
        check = (const unsigned char *)utf8_output;
        while (*check != '\0') {
            uint32_t cp;
            result = utf8_next(&check, &cp);
            if (result != SMS_PDU_OK) return result;
            (void)cp;
        }
        if (utf8_length != NULL) *utf8_length = byte_count;
        PDU_LOG("8BIT decode: restored %zu UTF-8 byte(s)\n", byte_count);
        return SMS_PDU_OK;
    }

    if (encoding == SMS_PDU_GSM7) {
        uint32_t bits = 0;
        unsigned int bit_count = 0;
        int escaped = 0;
        size_t expected_bytes = (data_units * 7 + 7) / 8;
        if (data_units == 0 && byte_count != 0) return SMS_PDU_INVALID_HEX;
        if (expected_bytes != byte_count) return SMS_PDU_INVALID_HEX;
        for (size_t septet_index = 0; septet_index < data_units; ++septet_index) {
            unsigned char septet;
            uint32_t cp;
            while (bit_count < 7) {
                unsigned char byte;
                result = hex_byte_at(hex_input, offset++, &byte);
                if (result != SMS_PDU_OK) return result;
                bits |= (uint32_t)byte << bit_count;
                bit_count += 8;
            }
            septet = (unsigned char)(bits & 0x7F);
            bits >>= 7;
            bit_count -= 7;
            if (escaped) {
                result = gsm7_to_code_point(septet, 1, &cp);
                escaped = 0;
            } else if (septet == 0x1B) {
                escaped = 1;
                continue;
            } else {
                result = gsm7_to_code_point(septet, 0, &cp);
            }
            if (result != SMS_PDU_OK) return result;
            PDU_LOG("GSM7 decode: septet 0x%02X -> U+%04lX\n", septet, (unsigned long)cp);
            result = append_utf8(cp, utf8_output, utf8_output_size - 1, &index);
            if (result != SMS_PDU_OK) return result;
        }
        if (escaped) return SMS_PDU_INVALID_UTF16;
        utf8_output[index] = '\0';
        if (utf8_length != NULL) *utf8_length = index;
        PDU_LOG("GSM7 decode: restored %zu UTF-8 byte(s) from %zu septet(s)\n", index, data_units);
        return SMS_PDU_OK;
    }
    return SMS_PDU_UNSUPPORTED_CHAR;
}
