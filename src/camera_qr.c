#include "camera_qr.h"
#include "quirc.h"
#include <stdbool.h>
#include <string.h>

enum camera_qr_result camera_qr_decode(struct quirc *decoder, char *activation, size_t size)
{
    enum camera_qr_result result = CAMERA_QR_NONE;
    for (int i = 0; i < quirc_count(decoder); i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(decoder, i, &code);
        quirc_decode_error_t error = quirc_decode(&code, &data);
        if (error == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); error = quirc_decode(&code, &data); }
        if (error) continue;
        bool valid = data.payload_len > 6 && data.payload_len < (int)size &&
                     !memcmp(data.payload, "LPA:1$", 6);
        for (int j = 0; valid && j < data.payload_len; j++)
            if (data.payload[j] <= 32 || data.payload[j] == 127) valid = false;
        const uint8_t *separator = valid ? memchr(data.payload + 6, '$', data.payload_len - 6) : NULL;
        valid = valid && separator && separator > data.payload + 6 &&
                separator + 1 < data.payload + data.payload_len && separator[1] != '$';
        if (valid) {
            memcpy(activation, data.payload, data.payload_len);
            activation[data.payload_len] = 0;
            return CAMERA_QR_ACTIVATION;
        }
        result = CAMERA_QR_OTHER;
    }
    return result;
}
