#define _GNU_SOURCE
#include "camera_capture.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <jpeglib.h>
#include "quirc.h"
#include "camera_qr.h"
#define __user
#include "camera_vendor/sprd_camsys.h"
#define sensor_prepare sensor_prepare_gc05a2
#include "camera_vendor/gc05a2-control.h"
#undef sensor_prepare
#include "camera_vendor/gc5035-control.h"
#include "camera_vendor/sp5508-control.h"

enum sensor_model { SENSOR_GC05A2, SENSOR_GC5035, SENSOR_SP5508 };

#define WIDTH 2592
#define HEIGHT 1944
#define BYTES (WIDTH * HEIGHT * 2)
#define BUFFERS 3
#define PHOTO "/tmp/camera.jpg"
static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    bool running, stop, photo, fresh, scan, restart, next_scan;
    unsigned zoom, sensor_x_fraction, sensor_y_fraction;
    struct camera_status status;
    uint32_t pixels[CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT];
    struct timespec progress;
} capture = {.lock = PTHREAD_MUTEX_INITIALIZER, .sensor_x_fraction = 10000, .sensor_y_fraction = 10000};

static void interrupt_read(int sig) { (void)sig; }

static void failure_locked(const char *what, int error)
{
    if (!capture.status.failed) {
        capture.status.failed = true;
        snprintf(capture.status.error, sizeof capture.status.error, "%s: %s", what, strerror(error));
    }
    capture.status.ready = false;
    capture.status.saving = false;
}

static void failure(const char *what)
{
    int error = errno;
    pthread_mutex_lock(&capture.lock);
    failure_locked(what, error);
    pthread_mutex_unlock(&capture.lock);
}

static bool stopping(void)
{
    pthread_mutex_lock(&capture.lock);
    bool stop = capture.stop;
    pthread_mutex_unlock(&capture.lock);
    return stop;
}

static int command(int fd, unsigned long op, void *arg, const char *name)
{
    if (ioctl(fd, op, arg) < 0) { failure(name); return -1; }
    return 0;
}

struct color_processing {
    double red, blue;
    bool balanced;
    uint8_t lookup[3][1024];
};

static void color_tables(struct color_processing *color)
{
    const double gains[] = {color->red, 1, color->blue};
    for (int c = 0; c < 3; c++) for (int i = 0; i < 1024; i++) {
        double linear = (i - 64) * gains[c] / 959;
        if (linear < 0) linear = 0;
        if (linear > 1) linear = 1;
        double srgb = linear <= 0.0031308 ? linear * 12.92 : 1.055 * pow(linear, 1 / 2.4) - 0.055;
        color->lookup[c][i] = (uint8_t)lround(srgb * 255);
    }
}

static void zoom_crop(unsigned zoom, int *x, int *y, int *width, int *height)
{
    *width = WIDTH * 100 / (int)zoom;
    *height = HEIGHT * 100 / (int)zoom;
    *x = ((WIDTH - *width) / 2) & ~1;
    *y = ((HEIGHT - *height) / 2) & ~1;
}

/* Use unsaturated midtones; near-black samples mostly measure read noise.
 * Average similar-channel samples to avoid metering a saturated colored object. */
static void white_balance(const uint16_t *raw, unsigned zoom, struct color_processing *color)
{
    double sums[3] = {0};
    unsigned samples = 0;
    int x0, y0, width, height;
    zoom_crop(zoom, &x0, &y0, &width, &height);
    for (int y = y0; y < y0 + height - 2; y += 16)
        for (int x = x0; x < x0 + width - 2; x += 16) {
        unsigned r = raw[y * WIDTH + x + 1];
        unsigned g = (raw[y * WIDTH + x] + raw[(y + 1) * WIDTH + x + 1]) / 2;
        unsigned b = raw[(y + 1) * WIDTH + x];
        if (r <= 80 || g <= 80 || b <= 80 || r >= 960 || g >= 960 || b >= 960) continue;
        double high = fmax(r - 64, fmax(g - 64, b - 64));
        double low = fmin(r - 64, fmin(g - 64, b - 64));
        if (high > low * 3) continue;
        sums[0] += r - 64; sums[1] += g - 64; sums[2] += b - 64;
        samples++;
    }
    if (!samples) return;
    double red = sums[1] / sums[0], blue = sums[1] / sums[2];
    if (color->balanced) {
        color->red += (red - color->red) * 0.15;
        color->blue += (blue - color->blue) * 0.15;
    } else {
        color->red = red; color->blue = blue; color->balanced = true;
    }
    color_tables(color);
}

/* Bilinear GRBG demosaic retains each pixel's measured channel. Reflecting
 * the border preserves Bayer parity and avoids a colored edge. */
static unsigned sample(const uint16_t *raw, int x, int y)
{
    if (x < 0) x = -x;
    if (x >= WIDTH) x = 2 * WIDTH - 2 - x;
    if (y < 0) y = -y;
    if (y >= HEIGHT) y = 2 * HEIGHT - 2 - y;
    unsigned value = raw[y * WIDTH + x];
    return value > 1023 ? 1023 : value;
}

static void rgb(const uint16_t *raw, int x, int y, const struct color_processing *color, uint8_t *out)
{
    unsigned r, g, b;
    unsigned horizontal = (sample(raw, x - 1, y) + sample(raw, x + 1, y)) / 2;
    unsigned vertical = (sample(raw, x, y - 1) + sample(raw, x, y + 1)) / 2;
    if ((x & 1) == (y & 1)) {
        g = sample(raw, x, y);
        r = (y & 1) ? vertical : horizontal;
        b = (y & 1) ? horizontal : vertical;
    } else {
        unsigned diagonal = (sample(raw, x - 1, y - 1) + sample(raw, x + 1, y - 1) +
                             sample(raw, x - 1, y + 1) + sample(raw, x + 1, y + 1)) / 4;
        g = (horizontal + vertical) / 2;
        r = (y & 1) ? diagonal : sample(raw, x, y);
        b = (y & 1) ? sample(raw, x, y) : diagonal;
    }
    out[0] = color->lookup[0][r];
    out[1] = color->lookup[1][g];
    out[2] = color->lookup[2][b];
}

/* Exposure is measured in lines; gain uses 1024 for unity across sensors. */
static int auto_exposure(int sensor, const uint16_t *raw, unsigned zoom, unsigned *exposure, unsigned *gain, enum sensor_model model)
{
    if (model == SENSOR_GC5035) return 0;
    unsigned histogram[1024] = {0}, samples = 0;
    int x0, y0, width, height;
    zoom_crop(zoom, &x0, &y0, &width, &height);
    for (int y = y0; y < y0 + height - 2; y += 32) for (int x = x0; x < x0 + width - 2; x += 32) {
        unsigned green = (raw[y * WIDTH + x] + raw[(y + 1) * WIDTH + x + 1]) / 2;
        histogram[green > 1023 ? 1023 : green]++;
        samples++;
    }
    /* Meter the upper midtones so small lamps do not darken the whole scene. */
    unsigned count = 0, level = 0;
    for (; level < 1023; level++) {
        count += histogram[level];
        if (count >= samples * 7 / 10) break;
    }
    double signal = level > 64 ? level - 64 : 1;
    double ratio = 160.0 / signal;
    if (ratio > 0.85 && ratio < 1.18) return 0;
    ratio = sqrt(ratio);
    if (ratio > 2) ratio = 2;
    if (ratio < 0.5) ratio = 0.5;
    double product = *exposure * (*gain / 1024.0) * ratio;
    unsigned min_lines = model == SENSOR_SP5508 ? 8 : 4;
    unsigned max_lines = model == SENSOR_SP5508 ? 1981 : 2016;
    unsigned max_gain = model == SENSOR_SP5508 ? 15872 : 16384;
    if (product < min_lines) product = min_lines;
    if (product > max_lines * (max_gain / 1024.0)) product = max_lines * (max_gain / 1024.0);
    unsigned lines = product > max_lines ? max_lines : (unsigned)product;
    unsigned step = model == SENSOR_SP5508 ? 64 : 8;
    unsigned code = (unsigned)(product / lines * 1024) / step * step;
    if (code < 1024) code = 1024;
    if (code > max_gain) code = max_gain;
    if (lines == *exposure && code == *gain) return 0;
    if (model == SENSOR_SP5508) {
        if (sensor_exposure_sp5508(sensor, lines, code)) return -1;
    } else if (sensor_write(sensor, 0x0202, lines >> 8) || sensor_write(sensor, 0x0203, lines & 255) ||
        sensor_write(sensor, 0x0204, code >> 8) || sensor_write(sensor, 0x0205, code & 0xf8)) return -1;
    *exposure = lines;
    *gain = code;
    fprintf(stderr, "camera AE: level=%u exposure=%u gain=%u\n", level, lines, code);
    return 0;
}

static void scan_frame(struct quirc *decoder, const uint16_t *raw, unsigned zoom,
                       unsigned sensor_x_fraction, unsigned sensor_y_fraction)
{
    int w, h;
    uint8_t *gray = quirc_begin(decoder, &w, &h);
    int crop_width = WIDTH * 100 / (int)zoom;
    int crop_height = HEIGHT * 100 / (int)zoom;
    int side = (int)((uint64_t)crop_width * sensor_x_fraction / 10000);
    int side_y = (int)((uint64_t)crop_height * sensor_y_fraction / 10000);
    if (side_y < side) side = side_y;
    int x0 = (WIDTH - side) / 2;
    int y0 = (HEIGHT - side) / 2;
    /* Decode only the centered viewfinder square; 2x2 green sampling avoids Bayer color artifacts. */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int sx = x0 + x * side / w;
        int sy = y0 + y * side / h;
        unsigned value = (raw[sy * WIDTH + sx] + raw[(sy + 1) * WIDTH + sx + 1]) / 2;
        gray[y * w + x] = value > 1023 ? 255 : value / 4;
    }
    quirc_end(decoder);
    char activation[sizeof capture.status.activation];
    char payload[sizeof capture.status.qr] = {0};
    enum camera_qr_result result = camera_qr_decode(decoder, activation, sizeof activation,
                                                     payload, sizeof payload);
    pthread_mutex_lock(&capture.lock);
    capture.status.qr[0] = 0;
    if (payload[0]) memcpy(capture.status.qr, payload, strlen(payload) + 1);
    if (result == CAMERA_QR_ACTIVATION)
        memcpy(capture.status.activation, activation, strlen(activation) + 1);
    else if (result == CAMERA_QR_OTHER)
        capture.status.other_qr = true;
    pthread_mutex_unlock(&capture.lock);
}

struct jpeg_failure { struct jpeg_error_mgr base; jmp_buf jump; };
static void jpeg_error(j_common_ptr common)
{
    struct jpeg_failure *error = (struct jpeg_failure *)common->err;
    longjmp(error->jump, 1);
}

static bool save_photo(const uint16_t *raw, const struct color_processing *color)
{
    FILE *file = fopen(PHOTO ".part", "wb");
    if (!file) return false;
    struct jpeg_compress_struct jpeg = {0};
    struct jpeg_failure error;
    uint8_t row[HEIGHT * 3];
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&jpeg);
        fclose(file);
        unlink(PHOTO ".part");
        errno = EIO;
        return false;
    }
    jpeg_create_compress(&jpeg);
    jpeg_stdio_dest(&jpeg, file);
    jpeg.image_width = HEIGHT;
    jpeg.image_height = WIDTH;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, 90, true);
    jpeg_start_compress(&jpeg, true);
    while (jpeg.next_scanline < WIDTH) {
        for (int x = 0; x < HEIGHT; x++)
            rgb(raw, (int)jpeg.next_scanline, HEIGHT - 1 - x, color, row + x * 3);
        JSAMPROW line = row;
        jpeg_write_scanlines(&jpeg, &line, 1);
    }
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    if (fclose(file) || rename(PHOTO ".part", PHOTO)) {
        int error = errno;
        unlink(PHOTO ".part");
        errno = error;
        return false;
    }
    return true;
}

static void *session(void *unused)
{
    (void)unused;
    int sensor = -1, image = -1, heap = -1, reserved = -1;
    int buffers[BUFFERS] = {-1, -1, -1};
    void *maps[BUFFERS] = {MAP_FAILED, MAP_FAILED, MAP_FAILED};
    enum sensor_model model = SENSOR_GC05A2;
    bool resources = false, streaming = false, csi_open = false;
    const char *modules[] = {"sprd_sensor", "sprd_flash_drv", "sprd_camera"};
    bool loaded[3] = {0};
    struct quirc *decoder = NULL;
    struct color_processing color = {.red = 1, .blue = 1};
    color_tables(&color);
    uint32_t *preview = malloc(sizeof capture.pixels);
    if (!preview) { failure("allocate preview"); goto out; }
    if (capture.scan) {
        decoder = quirc_new();
        if (!decoder || quirc_resize(decoder, 648, 648)) {
            errno = ENOMEM; failure("initialize QR scanner"); goto out;
        }
    }
    for (int i = 0; i < 3; i++) {
        char path[160];
        snprintf(path, sizeof path, "/sys/module/%s", modules[i]);
        if (!access(path, F_OK)) continue;
        snprintf(path, sizeof path, "/usr/libexec/e5-infoscreen/camera/%s.ko", modules[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { failure("open camera module"); goto out; }
        int ret = syscall(SYS_finit_module, fd, "", 0);
        int error = errno;
        close(fd);
        errno = error;
        if (ret < 0) { failure("load camera module"); goto out; }
        loaded[i] = true;
    }
    if (stopping()) goto out;
    sensor = open("/dev/sprd_sensor", O_RDWR | O_CLOEXEC);
    if (sensor < 0) { failure("open sensor"); goto out; }
    char profile[64] = {0};
    FILE *profile_file = fopen("/etc/e5/device-profile", "r");
    if (profile_file) { fgets(profile, sizeof profile, profile_file); fclose(profile_file); }
    bool e5 = !strncmp(profile, "e5", 2);
    if (e5) {
        model = SENSOR_SP5508;
        if (sensor_identify_sp5508(sensor) == 0) {
            if (sensor_configure_sp5508(sensor)) { failure("configure SP5508"); goto out; }
        } else {
            sensor_power_off_sp5508(sensor);
            model = SENSOR_GC5035;
            if (sensor_prepare_gc5035(sensor)) { failure("identify E5 camera (SP5508/GC5035)"); goto out; }
        }
    } else if (sensor_prepare_gc05a2(sensor)) {
        failure("initialize GC05A2"); goto out;
    }
    if (stopping()) goto out;
    image = open("/dev/sprd_image", O_RDWR | O_CLOEXEC);
    if (image < 0) { failure("open camera"); goto out; }
    uint32_t key = CAM_IOCTL_PRIVATE_KEY, mode = DCAM_CAPTURE_MODE_MULTIPLE;
    struct sprd_img_res res = {.width = WIDTH, .height = HEIGHT, .sensor_id = 0};
    if (command(image, SPRD_IMG_IO_SET_KEY, &key, "camera handshake") ||
        command(image, SPRD_IMG_IO_GET_DCAM_RES, &res, "camera resources")) goto out;
    resources = true;
    if (res.flag != 1) { errno = EIO; failure("camera resource flag"); goto out; }
    struct sprd_img_size size = {.w = WIDTH, .h = HEIGHT};
    struct sprd_img_rect rect = {.w = WIDTH, .h = HEIGHT};
    struct sprd_img_sensor_if input = {.if_type = DCAM_CAP_IF_CSI2, .img_fmt = DCAM_CAP_MODE_RAWRGB,
        .if_spec.mipi = {.bits_per_pxl = 10, .lane_num = 2}};
    struct sprd_img_parm output = {.scene_mode = DCAM_SCENE_MODE_CAPTURE, .need_isp_tool = 1,
        .sn_fmt = IMG_PIX_FMT_GREY, .pixel_fmt = IMG_PIX_FMT_GREY,
        .base_rect = rect, .crop_rect = rect, .dst_size = size};
    uint32_t channel = UINT32_MAX;
    struct sprd_dcam_raw_fmt format = {.sensor_raw_fmt = 1, .dcam_raw_fmt = 1};
    if (command(image, SPRD_IMG_IO_SET_MODE, &mode, "capture mode") ||
        command(image, SPRD_IMG_IO_SET_SENSOR_IF, &input, "sensor interface") ||
        command(image, SPRD_IMG_IO_SET_SENSOR_SIZE, &size, "sensor size") ||
        command(image, SPRD_IMG_IO_SET_SENSOR_TRIM, &rect, "sensor crop") ||
        command(image, SPRD_IMG_IO_SET_OUTPUT_SIZE, &output, "output size") ||
        command(image, SPRD_IMG_IO_GET_CH_ID, &channel, "RAW channel")) goto out;
    if (channel != 0) { errno = EINVAL; failure("RAW channel"); goto out; }
    if (command(image, SPRD_IMG_IO_SET_DCAM_RAW_FMT, &format, "RAW format") ||
        command(image, SPRD_IMG_IO_CHECK_FMT, &output, "pipeline")) goto out;
    heap = open("/dev/sprd_camera_heap", O_RDWR | O_CLOEXEC);
    if (heap < 0) { failure("open camera heap"); goto out; }
    output.buffer_count = 1;
    for (int i = 0; i < BUFFERS; i++) {
        struct dma_heap_allocation_data alloc = {.len = BYTES, .fd_flags = O_RDWR | O_CLOEXEC};
        if (command(heap, DMA_HEAP_IOCTL_ALLOC, &alloc, "allocate frame")) goto out;
        buffers[i] = alloc.fd;
        maps[i] = mmap(NULL, BYTES, PROT_READ, MAP_SHARED, buffers[i], 0);
        if (maps[i] == MAP_FAILED) { failure("map frame"); goto out; }
        output.fd_array[0] = buffers[i];
        if (command(image, SPRD_IMG_IO_SET_FRAME_ADDR, &output, "queue frame")) goto out;
    }
    struct dma_heap_allocation_data alloc = {.len = 4096, .fd_flags = O_RDWR | O_CLOEXEC};
    if (command(heap, DMA_HEAP_IOCTL_ALLOC, &alloc, "allocate reserve")) goto out;
    reserved = alloc.fd;
    output.fd_array[0] = reserved;
    output.is_reserved_buf = 1;
    if (command(image, SPRD_IMG_IO_SET_FRAME_ADDR, &output, "reserve frame")) goto out;
    if (stopping()) goto out;
    struct sensor_if_cfg_tag csi = {.if_type = 1, .phy_id = e5 ? 4 : 3, .lane_num = 2, .bps_per_lane = model == SENSOR_SP5508 ? 936 : e5 ? 876 : 896, .lane_seq = 0x0123};
    if (command(sensor, SENSOR_IO_IF_CFG, &csi, "open CSI")) goto out;
    csi_open = true;
    if (command(image, SPRD_IMG_IO_STREAM_ON, &mode, "start capture")) goto out;
    streaming = true;
    if ((model == SENSOR_SP5508 ? sensor_stream_sp5508(sensor, true) :
         model == SENSOR_GC5035 ? sensor_write8(sensor, 0x3e, 0x91) : sensor_write(sensor, 0x0100, 1))) {
        failure("sensor stream"); goto out;
    }
    unsigned frames = 0, exposure = model == SENSOR_SP5508 ? 1590 : 1842,
             gain = model == SENSOR_SP5508 ? 16320 : 1024;
    while (!stopping()) {
        struct sprd_img_read_op event = {.cmd = SPRD_IMG_GET_IMG_BUFFER};
        ssize_t n = read(image, &event, sizeof event);
        if (stopping()) break;
        if (n != sizeof event || event.evt != IMG_TX_DONE) {
            if (n >= 0) errno = EIO;
            failure("read camera frame"); break;
        }
        int slot = 0;
        while (slot < BUFFERS && buffers[slot] != event.parm.frame.mfd) slot++;
        if (slot == BUFFERS) { errno = EINVAL; failure("camera buffer"); break; }
        struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        if (command(buffers[slot], DMA_BUF_IOCTL_SYNC, &sync, "read frame memory")) break;
        bool frame_failed = false;
        if (++frames >= 3) {
            const uint16_t *raw = maps[slot];
            pthread_mutex_lock(&capture.lock);
            unsigned zoom = capture.zoom;
            unsigned sensor_x_fraction = capture.sensor_x_fraction;
            unsigned sensor_y_fraction = capture.sensor_y_fraction;
            pthread_mutex_unlock(&capture.lock);
            /* Keep metering off the hot path: the preview can use every frame,
             * while exposure and white balance only need a few updates per
             * second to converge. */
            if (frames % 6 == 0 && auto_exposure(sensor, raw, zoom, &exposure, &gain, model)) {
                failure("set exposure"); frame_failed = true; goto release_frame;
            }
            if (decoder && frames % 12 == 0)
                scan_frame(decoder, raw, zoom, sensor_x_fraction, sensor_y_fraction);
            if (frames == 3 || frames % 12 == 0) white_balance(raw, zoom, &color);
            int crop_width = WIDTH * 100 / (int)zoom;
            int crop_height = HEIGHT * 100 / (int)zoom;
            int crop_x = (WIDTH - crop_width) / 2;
            int crop_y = (HEIGHT - crop_height) / 2;
            for (int y = 0; y < CAMERA_PREVIEW_HEIGHT; y++) for (int x = 0; x < CAMERA_PREVIEW_WIDTH; x++) {
                uint8_t pixel[3];
                int raw_x = crop_x + y * crop_width / CAMERA_PREVIEW_HEIGHT;
                int raw_y = crop_y + (CAMERA_PREVIEW_WIDTH - 1 - x) * crop_height / CAMERA_PREVIEW_WIDTH;
                rgb(raw, raw_x, raw_y, &color, pixel);
                preview[y * CAMERA_PREVIEW_WIDTH + x] = 0xff000000u | (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
            }
            pthread_mutex_lock(&capture.lock);
            bool photo = capture.photo;
            capture.photo = false;
            capture.status.ready = true;
            memcpy(capture.pixels, preview, sizeof capture.pixels);
            capture.fresh = true;
            clock_gettime(CLOCK_MONOTONIC, &capture.progress);
            pthread_mutex_unlock(&capture.lock);
            if (photo) {
                if (!save_photo(raw, &color)) { failure("save " PHOTO); frame_failed = true; goto release_frame; }
                pthread_mutex_lock(&capture.lock);
                capture.status.saving = false;
                capture.status.saved = true;
                capture.status.photos++;
                pthread_mutex_unlock(&capture.lock);
            }
        }
release_frame:
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        if (command(buffers[slot], DMA_BUF_IOCTL_SYNC, &sync, "release frame memory")) break;
        if (frame_failed) break;
        output.is_reserved_buf = 0;
        output.fd_array[0] = buffers[slot];
        if (command(image, SPRD_IMG_IO_SET_FRAME_ADDR, &output, "requeue frame")) break;
    }
out:
    if (sensor >= 0) {
        if (model == SENSOR_SP5508) {
            if (streaming) sensor_stream_sp5508(sensor, false);
        } else if (model == SENSOR_GC5035) sensor_write8(sensor, 0x3e, 0);
        else sensor_write(sensor, 0x0100, 0);
    }
    uint32_t off = 0;
    if (streaming) command(image, SPRD_IMG_IO_STREAM_OFF, &off, "stop capture");
    if (csi_open) {
        struct sensor_if_cfg_tag close_csi = {.if_type = 1, .is_open = 1};
        command(sensor, SENSOR_IO_IF_CFG, &close_csi, "close CSI");
    }
    if (resources) {
        struct sprd_img_res release = {.sensor_id = 0};
        command(image, SPRD_IMG_IO_PUT_DCAM_RES, &release, "release camera");
    }
    for (int i = 0; i < BUFFERS; i++) {
        if (maps[i] != MAP_FAILED) munmap(maps[i], BYTES);
        if (buffers[i] >= 0) close(buffers[i]);
    }
    if (reserved >= 0) close(reserved);
    if (heap >= 0) close(heap);
    if (image >= 0) close(image);
    if (sensor >= 0) {
        if (model == SENSOR_SP5508) sensor_power_off_sp5508(sensor);
        else if (model == SENSOR_GC5035) {
            sensor_u32(sensor, SENSOR_IO_RST_LEVEL, 0);
            sensor_u32(sensor, SENSOR_IO_PD, 0);
            sensor_u32(sensor, SENSOR_IO_SET_MCLK, 0);
            sensor_u32(sensor, SENSOR_IO_SET_AVDD, 13);
            sensor_u32(sensor, SENSOR_IO_SET_DVDD, 13);
            sensor_u32(sensor, SENSOR_IO_SET_IOVDD, 13);
        } else sensor_power_off(sensor);
        close(sensor);
    }
    for (int i = 2; i >= 0; i--) if (loaded[i] && syscall(SYS_delete_module, modules[i], O_NONBLOCK) < 0)
        failure("unload camera module");
    if (decoder) quirc_destroy(decoder);
    free(preview);
    pthread_mutex_lock(&capture.lock);
    capture.status.ready = capture.status.saving = false;
    pthread_mutex_unlock(&capture.lock);
    return NULL;
}

static void reset_session(bool scan)
{
    capture.stop = capture.photo = capture.fresh = false;
    capture.scan = scan;
    capture.status = (struct camera_status){0};
    clock_gettime(CLOCK_MONOTONIC, &capture.progress);
}

static void *run(void *unused)
{
    for (;;) {
        session(unused);
        pthread_mutex_lock(&capture.lock);
        if (!capture.restart) {
            capture.running = false;
            pthread_mutex_unlock(&capture.lock);
            return NULL;
        }
        reset_session(capture.next_scan);
        capture.restart = false;
        pthread_mutex_unlock(&capture.lock);
    }
}

bool camera_capture_start(bool scan)
{
    struct sigaction action = {.sa_handler = interrupt_read};
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, NULL);
    pthread_mutex_lock(&capture.lock);
    if (capture.running) {
        capture.restart = true;
        capture.next_scan = scan;
        capture.stop = true;
        pthread_kill(capture.thread, SIGUSR2);
        pthread_mutex_unlock(&capture.lock);
        return true;
    }
    reset_session(scan);
    /* The decoder and JPEG encoder share this worker's stack. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int ret = pthread_create(&capture.thread, &attr, run, NULL);
    pthread_attr_destroy(&attr);
    capture.running = ret == 0;
    if (ret) failure_locked("camera thread", ret);
    pthread_mutex_unlock(&capture.lock);
    if (ret) errno = ret;
    return ret == 0;
}

void camera_capture_stop(void)
{
    pthread_mutex_lock(&capture.lock);
    capture.restart = false;
    capture.stop = true;
    if (capture.running) pthread_kill(capture.thread, SIGUSR2);
    pthread_mutex_unlock(&capture.lock);
}

bool camera_capture_busy(void)
{
    pthread_mutex_lock(&capture.lock);
    bool busy = capture.running;
    pthread_mutex_unlock(&capture.lock);
    return busy;
}

void camera_capture_photo(void)
{
    pthread_mutex_lock(&capture.lock);
    if (!capture.stop && capture.status.ready && !capture.status.failed && !capture.status.saving) {
        capture.photo = capture.status.saving = true;
        capture.status.saved = false;
    }
    pthread_mutex_unlock(&capture.lock);
}

void camera_capture_set_zoom(unsigned zoom, unsigned sensor_x_fraction, unsigned sensor_y_fraction)
{
    pthread_mutex_lock(&capture.lock);
    capture.zoom = zoom;
    capture.sensor_x_fraction = sensor_x_fraction;
    capture.sensor_y_fraction = sensor_y_fraction;
    pthread_mutex_unlock(&capture.lock);
}

bool camera_capture_update(uint32_t *pixels, struct camera_status *status)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&capture.lock);
    bool fresh = capture.fresh;
    if (fresh && pixels) memcpy(pixels, capture.pixels, sizeof capture.pixels);
    capture.fresh = false;
    if (capture.running && !capture.stop && !capture.status.failed && now.tv_sec - capture.progress.tv_sec > 5) {
        capture.status.failed = true;
        capture.status.ready = capture.status.saving = false;
        snprintf(capture.status.error, sizeof capture.status.error, "Camera frame timed out");
        capture.stop = true;
        pthread_kill(capture.thread, SIGUSR2);
    }
    if (capture.running && capture.stop) pthread_kill(capture.thread, SIGUSR2);
    *status = capture.restart ? (struct camera_status){0} : capture.status;
    if (capture.restart) fresh = false;
    pthread_mutex_unlock(&capture.lock);
    return fresh;
}
