#pragma once
#include <stddef.h>
struct quirc;
enum camera_qr_result { CAMERA_QR_NONE, CAMERA_QR_OTHER, CAMERA_QR_ACTIVATION };
enum camera_qr_result camera_qr_decode(struct quirc *decoder, char *activation, size_t size,
                                       char *payload, size_t payload_size);
