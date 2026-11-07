#define _GNU_SOURCE
#include "api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 3
#define API_PORT 8088
#define EVENTS_PORT 8089
#define TIMEOUT_S 25

struct request {
    struct request *next;
    const char *method;
    char *path;
    char *body;
    bool text;
    api_cb cb;
    api_text_cb text_cb;
    api_event_cb event_cb;
    void *ctx;
    int status;
    char *response;
    cJSON *json;
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    struct request *todo, **todo_tail;
    struct request *done, **done_tail;
    int wake[2];
} q = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ready = PTHREAD_COND_INITIALIZER,
};

static int tcp_connect(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool send_all(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static char *http(const char *method, const char *path, const char *body, int *status)
{
    *status = 0;
    int fd = tcp_connect(API_PORT);
    if (fd < 0)
        return NULL;
    size_t body_len = body ? strlen(body) : 0;
    char *head = NULL;
    int head_len = asprintf(&head,
                            "%s /api%s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n"
                            "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                            method, path, API_PORT, body_len);
    bool sent = head_len > 0 && send_all(fd, head, (size_t)head_len) && send_all(fd, body ? body : "", body_len);
    free(head);
    if (!sent) {
        close(fd);
        return NULL;
    }
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (cap - len < 4096)
            buf = realloc(buf, cap *= 2);
        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            len = 0;
            break;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = 0;
    char *split = strstr(buf, "\r\n\r\n");
    if (!split || sscanf(buf, "HTTP/%*s %d", status) != 1) {
        *status = 0;
        free(buf);
        return NULL;
    }
    memmove(buf, split + 4, len - (size_t)(split + 4 - buf) + 1);
    return buf;
}

static void finish(struct request *r)
{
    pthread_mutex_lock(&q.lock);
    r->next = NULL;
    *q.done_tail = r;
    q.done_tail = &r->next;
    pthread_mutex_unlock(&q.lock);
    char byte = 1;
    while (write(q.wake[1], &byte, 1) < 0 && errno == EINTR) {
    }
}

static void *worker(void *unused)
{
    for (;;) {
        pthread_mutex_lock(&q.lock);
        while (!q.todo)
            pthread_cond_wait(&q.ready, &q.lock);
        struct request *r = q.todo;
        q.todo = r->next;
        if (!q.todo)
            q.todo_tail = &q.todo;
        pthread_mutex_unlock(&q.lock);

        r->response = http(r->method, r->path, r->body, &r->status);
        if (r->response && !r->text)
            r->json = cJSON_Parse(r->response);
        finish(r);
    }
    return NULL;
}

void api_init(void)
{
    if (pipe2(q.wake, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("pipe2");
        exit(1);
    }
    q.todo_tail = &q.todo;
    q.done_tail = &q.done;
    for (int i = 0; i < WORKERS; i++) {
        pthread_t thread;
        pthread_create(&thread, NULL, worker, NULL);
        pthread_detach(thread);
    }
}

int api_fd(void)
{
    return q.wake[0];
}

/* Replies held back while the screen is in motion, in arrival order. */
static struct request *held, **held_tail = &held;

static void deliver(struct request *r)
{
    if (r->event_cb)
        r->event_cb(r->json);
    else if (r->text_cb)
        r->text_cb(r->status, r->response, r->ctx);
    else if (r->cb)
        r->cb(r->status, r->json, r->ctx);
    cJSON_Delete(r->json);
    free(r->response);
    free(r->path);
    free(r->body);
    free(r);
}

bool api_held(void)
{
    return held;
}

void api_dispatch(bool hold)
{
    char drain[64];
    while (read(q.wake[0], drain, sizeof drain) > 0) {
    }
    pthread_mutex_lock(&q.lock);
    struct request *list = q.done;
    q.done = NULL;
    q.done_tail = &q.done;
    pthread_mutex_unlock(&q.lock);
    if (!hold && held) {
        *held_tail = list;
        list = held;
        held = NULL;
        held_tail = &held;
    }
    while (list) {
        struct request *r = list;
        list = r->next;
        if (hold && !r->event_cb) {
            r->next = NULL;
            *held_tail = r;
            held_tail = &r->next;
        } else {
            deliver(r);
        }
    }
}

static void submit(struct request *r)
{
    pthread_mutex_lock(&q.lock);
    *q.todo_tail = r;
    q.todo_tail = &r->next;
    pthread_cond_signal(&q.ready);
    pthread_mutex_unlock(&q.lock);
}

void api_get(const char *path, api_cb cb, void *ctx)
{
    struct request *r = calloc(1, sizeof *r);
    *r = (struct request){.method = "GET", .path = strdup(path), .cb = cb, .ctx = ctx};
    submit(r);
}

void api_get_text(const char *path, api_text_cb cb, void *ctx)
{
    struct request *r = calloc(1, sizeof *r);
    *r = (struct request){.method = "GET", .path = strdup(path), .text = true, .text_cb = cb, .ctx = ctx};
    submit(r);
}

void api_post(const char *path, cJSON *body, api_cb cb, void *ctx)
{
    struct request *r = calloc(1, sizeof *r);
    *r = (struct request){.method = "POST", .path = strdup(path), .cb = cb, .ctx = ctx,
                          .body = body ? cJSON_PrintUnformatted(body) : strdup("{}")};
    cJSON_Delete(body);
    submit(r);
}

bool api_read_ok(int status, const cJSON *reply)
{
    if (status < 200 || status >= 300 || !reply)
        return false;
    return !(cJSON_GetArraySize(reply) == 1 && cJSON_GetObjectItem(reply, "error"));
}

bool api_reply_ok(const cJSON *reply)
{
    const cJSON *ok = cJSON_GetObjectItem(reply, "ok");
    return cJSON_IsTrue(ok) || (cJSON_IsNumber(ok) && ok->valuedouble != 0);
}

/* Call state: the voice service's event stream, or the plugin's live read
 * every 350 ms while the stream is unavailable. */
static api_event_cb call_event_cb;

static void deliver_event(cJSON *event)
{
    struct request *r = calloc(1, sizeof *r);
    r->event_cb = call_event_cb;
    r->json = event;
    finish(r);
}

static bool calls_payload(const cJSON *json)
{
    const cJSON *error = cJSON_GetObjectItem(json, "error");
    return json && (!error || cJSON_IsNull(error)) && cJSON_IsArray(cJSON_GetObjectItem(json, "calls"));
}

static void stream_events(void)
{
    int fd = tcp_connect(EVENTS_PORT);
    if (fd < 0)
        return;
    struct timeval tv = {0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char head[160];
    int head_len = snprintf(head, sizeof head,
                            "GET /events HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://127.0.0.1:%d\r\n\r\n",
                            EVENTS_PORT, API_PORT);
    if (!send_all(fd, head, (size_t)head_len)) {
        close(fd);
        return;
    }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    bool headers = true;
    for (;;) {
        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += (size_t)n;
        buf[len] = 0;
        char *start = buf, *end;
        if (headers) {
            end = strstr(start, "\r\n\r\n");
            if (!end)
                continue;
            if (strncmp(buf, "HTTP/1.1 200", 12))
                break;
            start = end + 4;
            headers = false;
        }
        while ((end = strstr(start, "\n\n"))) {
            *end = 0;
            if (!strncmp(start, "data: ", 6)) {
                cJSON *event = cJSON_Parse(start + 6);
                if (!calls_payload(event)) {
                    cJSON_Delete(event);
                    goto out;
                }
                deliver_event(event);
            }
            start = end + 2;
        }
        len -= (size_t)(start - buf);
        memmove(buf, start, len + 1);
        if (len + 1 >= cap)
            break;
    }
out:
    free(buf);
    close(fd);
}

static void *events_thread(void *unused)
{
    struct timespec last_try = {0};
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last_try.tv_sec >= 3) {
            last_try = now;
            stream_events();
        }
        int status;
        char *body = http("GET", "/plugins/phone/live", NULL, &status);
        cJSON *json = status == 200 && body ? cJSON_Parse(body) : NULL;
        free(body);
        if (calls_payload(json))
            deliver_event(json);
        else
            cJSON_Delete(json);
        usleep(350000);
    }
    return NULL;
}

void api_call_events(api_event_cb cb)
{
    call_event_cb = cb;
    pthread_t thread;
    pthread_create(&thread, NULL, events_thread, NULL);
    pthread_detach(thread);
}
