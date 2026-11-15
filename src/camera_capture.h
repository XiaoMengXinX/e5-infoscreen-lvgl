#pragma once
#include <stdbool.h>
#include <stdint.h>
#define CAMERA_PREVIEW_WIDTH 240
#define CAMERA_PREVIEW_HEIGHT 320
struct camera_status {
    bool ready, saving, saved, failed;
    unsigned photos;
    char error[160];
    char activation[4096];
    bool other_qr;
};
bool camera_capture_start(bool scan);
/* Stop is asynchronous; keep calling update until busy returns false. */
void camera_capture_stop(void);
bool camera_capture_busy(void);
void camera_capture_photo(void);
bool camera_capture_update(uint32_t *pixels, struct camera_status *status);
