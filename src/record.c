/* Screen recording: frames are copied off the display as it flips, up to its 60 per
 * second, and a worker thread compresses them to JPEG in a QuickTime movie, the format
 * QuickTime and ffmpeg-based players both open without extra codecs. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "app.h"
#include "drm.h"
#include "theme.h"
#include "ui.h"

#define RECORD_PATH "/tmp/record.mov"
#define SLOTS 4
#define FRAME_MS 16
#define LIMIT_MS 60000

struct frame {
    uint8_t *pixels;
    uint32_t ms;
};

/* Shared with the worker under the lock. */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct frame slots[SLOTS];
    int head, count;
    bool stopping;
} queue = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

/* Owned by the worker until it is joined. */
static struct {
    FILE *file;
    long mdat;
    uint32_t *offsets, *sizes, *times;
    size_t count, capacity;
    bool failed;
} movie;

static pthread_t worker;
static bool recording;
static uint32_t width, height, started, captured, end_ms;
static lv_timer_t *timer;

/* ---------- QuickTime movie ---------- */

static void u32(uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 4, movie.file);
}

static void u16(uint32_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    fwrite(b, 1, 2, movie.file);
}

static void zeros(int n)
{
    while (n--)
        fputc(0, movie.file);
}

static long atom(const char *type)
{
    long at = ftell(movie.file);
    u32(0);
    fwrite(type, 1, 4, movie.file);
    return at;
}

static void close_atom(long at)
{
    long end = ftell(movie.file);
    fseek(movie.file, at, SEEK_SET);
    u32((uint32_t)(end - at));
    fseek(movie.file, end, SEEK_SET);
}

static void matrix(void)
{
    static const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++)
        u32(m[i]);
}

static uint32_t duration_of(size_t i)
{
    uint32_t next = i + 1 < movie.count ? movie.times[i + 1] : end_ms;
    return next > movie.times[i] ? next - movie.times[i] : 1;
}

static void write_moov(void)
{
    uint32_t total = end_ms > 0 ? end_ms : 1;
    long moov = atom("moov");
    long a = atom("mvhd");
    u32(0), u32(0), u32(0), u32(1000), u32(total), u32(0x10000), u16(0x100);
    zeros(10);
    matrix();
    zeros(24);
    u32(2);
    close_atom(a);

    long trak = atom("trak");
    a = atom("tkhd");
    u32(3), u32(0), u32(0), u32(1), u32(0), u32(total);
    zeros(8);
    u16(0), u16(0), u16(0), u16(0);
    matrix();
    u32(width << 16), u32(height << 16);
    close_atom(a);

    long mdia = atom("mdia");
    a = atom("mdhd");
    u32(0), u32(0), u32(0), u32(1000), u32(total), u16(0), u16(0);
    close_atom(a);
    a = atom("hdlr");
    u32(0);
    fwrite("mhlrvide", 1, 8, movie.file);
    zeros(12 + 1);
    close_atom(a);

    long minf = atom("minf");
    a = atom("vmhd");
    u32(1), u16(0x40), u16(0x8000), u16(0x8000), u16(0x8000);
    close_atom(a);
    a = atom("hdlr");
    u32(0);
    fwrite("dhlralis", 1, 8, movie.file);
    zeros(12 + 1);
    close_atom(a);
    long dinf = atom("dinf");
    a = atom("dref");
    u32(0), u32(1), u32(12);
    fwrite("alis", 1, 4, movie.file);
    u32(1);
    close_atom(a);
    close_atom(dinf);

    long stbl = atom("stbl");
    a = atom("stsd");
    u32(0), u32(1);
    long entry = atom("jpeg");
    zeros(6);
    u16(1), u16(0), u16(0), u32(0), u32(0), u32(512), u16(width), u16(height);
    u32(72 << 16), u32(72 << 16), u32(0), u16(1);
    static const char name[32] = "\x0cPhoto - JPEG";
    fwrite(name, 1, sizeof name, movie.file);
    u16(24), u16(0xffff);
    close_atom(entry);
    close_atom(a);

    a = atom("stts");
    u32(0);
    long count_at = ftell(movie.file);
    u32(0);
    uint32_t runs = 0;
    for (size_t i = 0; i < movie.count;) {
        uint32_t d = duration_of(i), n = 0;
        while (i < movie.count && duration_of(i) == d)
            i++, n++;
        u32(n), u32(d);
        runs++;
    }
    long end = ftell(movie.file);
    fseek(movie.file, count_at, SEEK_SET);
    u32(runs);
    fseek(movie.file, end, SEEK_SET);
    close_atom(a);

    a = atom("stsc");
    u32(0), u32(1), u32(1), u32(1), u32(1);
    close_atom(a);
    a = atom("stsz");
    u32(0), u32(0), u32((uint32_t)movie.count);
    for (size_t i = 0; i < movie.count; i++)
        u32(movie.sizes[i]);
    close_atom(a);
    a = atom("stco");
    u32(0), u32((uint32_t)movie.count);
    for (size_t i = 0; i < movie.count; i++)
        u32(movie.offsets[i]);
    close_atom(a);

    close_atom(stbl);
    close_atom(minf);
    close_atom(mdia);
    close_atom(trak);
    close_atom(moov);
}

static void add_frame(const struct frame *f)
{
    if (movie.count == movie.capacity) {
        movie.capacity = movie.capacity ? movie.capacity * 2 : 512;
        movie.offsets = realloc(movie.offsets, movie.capacity * sizeof *movie.offsets);
        movie.sizes = realloc(movie.sizes, movie.capacity * sizeof *movie.sizes);
        movie.times = realloc(movie.times, movie.capacity * sizeof *movie.times);
    }
    long at = ftell(movie.file);
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr error;
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, movie.file);
    c.image_width = width;
    c.image_height = height;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_BGRX;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 80, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < height) {
        JSAMPROW row = f->pixels + (size_t)c.next_scanline * width * 4;
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    movie.offsets[movie.count] = (uint32_t)at;
    movie.sizes[movie.count] = (uint32_t)(ftell(movie.file) - at);
    movie.times[movie.count++] = f->ms;
}

static void *work(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&queue.lock);
        while (!queue.count && !queue.stopping)
            pthread_cond_wait(&queue.cond, &queue.lock);
        if (!queue.count) {
            pthread_mutex_unlock(&queue.lock);
            break;
        }
        struct frame f = queue.slots[queue.head];
        pthread_mutex_unlock(&queue.lock);
        add_frame(&f);
        pthread_mutex_lock(&queue.lock);
        queue.head = (queue.head + 1) % SLOTS;
        queue.count--;
        pthread_mutex_unlock(&queue.lock);
    }
    close_atom(movie.mdat);
    write_moov();
    movie.failed = ferror(movie.file) != 0;
    movie.failed |= fclose(movie.file) != 0;
    return NULL;
}

/* ---------- capture ---------- */

static void tick(lv_timer_t *t)
{
    uint32_t now = lv_tick_elaps(started);
    if (now >= LIMIT_MS) {
        record_stop();
        return;
    }
    shell_record_indicator((int)(now / 1000));
    /* Only a flipped frame is new; a still screen just lengthens the last one. */
    if (drm_display_frames() == captured)
        return;
    pthread_mutex_lock(&queue.lock);
    if (queue.count < SLOTS) {
        struct frame *f = &queue.slots[(queue.head + queue.count) % SLOTS];
        uint32_t pitch;
        const uint8_t *front = drm_display_front(&pitch);
        for (uint32_t y = 0; y < height; y++)
            memcpy(f->pixels + (size_t)y * width * 4, front + (size_t)y * pitch, (size_t)width * 4);
        f->ms = now;
        queue.count++;
        captured = drm_display_frames();
        pthread_cond_signal(&queue.cond);
    }
    pthread_mutex_unlock(&queue.lock);
}

bool record_active(void)
{
    return recording;
}

static void start(void)
{
    drm_display_size(&width, &height);
    movie = (typeof(movie)){.file = fopen(RECORD_PATH, "wb")};
    if (!movie.file) {
        toast("%s", L("无法创建录屏文件", "Cannot create the recording"));
        return;
    }
    for (int i = 0; i < SLOTS; i++)
        queue.slots[i].pixels = malloc((size_t)width * height * 4);
    queue.head = queue.count = 0;
    queue.stopping = false;
    u32(20);
    fwrite("ftypqt  ", 1, 8, movie.file);
    u32(0x20050300);
    fwrite("qt  ", 1, 4, movie.file);
    movie.mdat = atom("mdat");
    pthread_create(&worker, NULL, work, NULL);
    recording = true;
    started = lv_tick_get();
    /* The screen without the prompt is the first frame. */
    captured = drm_display_frames();
    timer = lv_timer_create(tick, FRAME_MS, NULL);
    shell_record_indicator(0);
}

void record_stop(void)
{
    if (!recording)
        return;
    recording = false;
    lv_timer_delete(timer);
    timer = NULL;
    end_ms = lv_tick_elaps(started);
    pthread_mutex_lock(&queue.lock);
    queue.stopping = true;
    pthread_cond_signal(&queue.cond);
    pthread_mutex_unlock(&queue.lock);
    pthread_join(worker, NULL);
    for (int i = 0; i < SLOTS; i++) {
        free(queue.slots[i].pixels);
        queue.slots[i].pixels = NULL;
    }
    free(movie.offsets);
    free(movie.sizes);
    free(movie.times);
    shell_record_indicator(-1);
    toast("%s", movie.failed ? L("录屏保存失败", "The recording could not be saved")
                             : L("录屏已保存到 " RECORD_PATH, "Recording saved to " RECORD_PATH));
}

/* ---------- prompt ---------- */

static void on_start(lv_event_t *e)
{
    dialog_close();
    start();
}

void record_prompt(void)
{
    if (dialog_is_open() || recording)
        return;
    if (app.blank)
        screen_set_blank(false);
    if (app.blank)
        return;
    dialog_open(L("屏幕录制", "Screen recording"),
                L("最长录制 1 分钟，同时按音量加和音量减结束\n保存到 " RECORD_PATH,
                  "Up to one minute; press both volume keys to stop\nSaved to " RECORD_PATH));
    lv_obj_t *go = dialog_button(L("开始录制", "Start recording"), on_start, NULL);
    lv_obj_set_style_bg_color(go, theme.poor, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(go, 0), lv_color_white(), 0);
    dialog_cancel();
}
