#include "camera_qr.h"
#include "quirc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *input = fopen(argv[1], "rb");
    assert(input);
    char magic[3];
    int w, h, max;
    assert(fscanf(input, "%2s %d %d %d", magic, &w, &h, &max) == 4);
    assert(!strcmp(magic, "P5") && max == 255);
    assert(fgetc(input) == '\n');
    struct quirc *decoder = quirc_new();
    assert(decoder && !quirc_resize(decoder, w, h));
    uint8_t *image = quirc_begin(decoder, NULL, NULL);
    assert(fread(image, 1, (size_t)w * h, input) == (size_t)w * h);
    fclose(input);
    quirc_end(decoder);
    char activation[4096] = "unchanged", payload[256] = "";
    enum camera_qr_result result = camera_qr_decode(decoder, activation, sizeof activation, payload, sizeof payload);
    if (!strcmp(argv[2], "other")) {
        assert(result == CAMERA_QR_OTHER && !strcmp(activation, "unchanged"));
    } else if (!strcmp(argv[2], "none")) {
        assert(result == CAMERA_QR_NONE && !strcmp(activation, "unchanged") && !*payload);
    } else {
        assert(result == CAMERA_QR_ACTIVATION && !strcmp(activation, argv[2]) && !strcmp(payload, argv[2]));
        assert(camera_qr_decode(decoder, activation, 6, payload, sizeof payload) == CAMERA_QR_OTHER);
    }
    quirc_destroy(decoder);
    return 0;
}
