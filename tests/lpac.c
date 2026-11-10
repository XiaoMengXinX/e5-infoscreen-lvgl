#define access test_access
#define execv test_execv
#include "../src/lpac.c"
#undef access
#undef execv

#include <assert.h>
#include <sys/socket.h>

static bool program_present, backend_present;
int test_access(const char *path, int mode)
{
    return (!strcmp(path, LPAC) ? program_present :
            !strcmp(path, SYSTEM_BACKEND) ? backend_present : false) ? 0 : -1;
}

static bool completed, result_ok;
static size_t result_length;
static char result_aid[33];
void idle_reset(void) {}

int test_execv(const char *path, char *const argv[])
{
    assert(!strcmp(path, "/usr/bin/python3"));
    assert(!strcmp(argv[1], "-c") && !strcmp(argv[2], LPAC_BRIDGE_SCRIPT));
    assert(!strcmp(argv[4], "0") || !strcmp(argv[4], "1"));
    assert(!strcmp(argv[6], "csim") || !strcmp(argv[6], "logical"));
    assert(argv[7] && argv[8]);
    if (!strcmp(argv[3], "auto"))
        puts("{\"type\":\"transport\",\"payload\":{\"mode\":\"at:curl\"}}");
    const char *aid = *argv[5] ? argv[5] : NULL;
    printf("{\"type\":\"lpa\",\"payload\":{\"code\":0,\"data\":\"%s\"}}\n", aid ? aid : "standard");
    fflush(stdout);
    _exit(0);
}

static void result(bool ok, const cJSON *data, const char *error, const char *detail)
{
    completed = true;
    result_ok = ok;
    result_length = cJSON_IsString(data) ? strlen(data->valuestring) : 0;
    if (cJSON_IsString(data))
        copy_text(result_aid, sizeof result_aid, data->valuestring);
}

static void receive(pid_t child, int fd)
{
    setpgid(child, child);
    run.pid = child;
    run.out = fd;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    run.count = 1;
    run.head = 0;
    run.q[0] = (struct job){.argv = calloc(2, sizeof(char *)), .done = result};
    run.q[0].argv[0] = (char *)LPAC;
    completed = false;
    for (int i = 0; !completed && i < 5000; i++) {
        output_poll();
        usleep(1000);
    }
    assert(completed);
}

static volatile sig_atomic_t interrupted;
static void cancel_signal(int sig) { interrupted = 1; }

static void expect_command(int fd, const char *expected)
{
    char command[2048] = {0};
    assert(read(fd, command, sizeof command - 1) > 0);
    if (strcmp(command, expected))
        fprintf(stderr, "Expected [%s], received [%s]\n", expected, command);
    assert(!strcmp(command, expected));
}

static void answer_at(const char *reply)
{
    void (*then)(const char *) = at.then;
    at.active = false;
    at.then = NULL;
    then(reply);
}

static void test_csim_bridge(void)
{
    int wire[2], output[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, wire) == 0 && pipe(output) == 0);
    run.dev = wire[0];
    fcntl(run.dev, F_SETFL, O_NONBLOCK);
    run.master = output[1];
    run.card = 1;
    run.csim = true;
    bridge_clear();
    copy_text(bridge.aid, sizeof bridge.aid, "ABCD");
    open_channel(on_opened);
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"0070000001\"\r\n");
    answer_at("+CSIM: 6,\"019000\"\r\nOK\r\n");
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=14,\"01A4040002ABCD\"\r\n");
    answer_at("+CSIM: 4,\"9000\"\r\nOK\r\n");
    assert(bridge.open && !strcmp(bridge.channel, "1"));
    expect_command(output[0], "+CCHO: 1\r\n\r\nOK\r\n");
    copy_text(bridge.apdu, sizeof bridge.apdu, "80CA000000");
    cgla_send();
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"81CA000000\"\r\n");
    answer_at("+CSIM: 4,\"9000\"\r\nOK\r\n");
    expect_command(output[0], "+CGLA: 4,\"9000\"\r\n\r\nOK\r\n");
    cchc_request();
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"0070800100\"\r\n");
    answer_at("+CSIM: 4,\"9000\"\r\nOK\r\n");
    assert(!bridge.open);
    expect_command(output[0], "OK\r\n");

    open_channel(on_opened);
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"0070000001\"\r\n");
    answer_at("+CSIM: 6,\"029000\"\r\nOK\r\n");
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=14,\"02A4040002ABCD\"\r\n");
    answer_at("+CSIM: 4,\"6A82\"\r\nOK\r\n");
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"0070800200\"\r\n");
    answer_at("+CSIM: 4,\"9000\"\r\nOK\r\n");
    expect_command(output[0], "+CME ERROR: 10\r\nERROR\r\n");
    assert(!bridge.open && !*bridge.channel);

    run.refreshing = true;
    run.refresh_ok = true;
    run.refresh_done = result;
    completed = false;
    open_channel(on_estk_channel);
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"0070000001\"\r\n");
    answer_at("+CSIM: 6,\"029000\"\r\nOK\r\n");
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=14,\"02A4040002ABCD\"\r\n");
    answer_at("+CSIM: 4,\"9000\"\r\nOK\r\n");
    expect_command(wire[1], "AT+SPACTCARD=1;+CSIM=10,\"0070800200\"\r\n");
    answer_at("+CSIM: 4,\"9000\"\r\nOK\r\n");
    assert(completed && result_ok && !run.refreshing);

    run.csim = false;
    open_channel(on_opened);
    expect_command(wire[1], "AT+SPACTCARD=1;+CCHO=\"ABCD\"\r\n");
    answer_at("+CCHO: 2\r\nOK\r\n");
    expect_command(output[0], "+CCHO: 2\r\n\r\nOK\r\n");
    cchc_request();
    expect_command(wire[1], "AT+SPACTCARD=1;+CCHC=2\r\n");
    answer_at("OK\r\n");
    expect_command(output[0], "OK\r\n");
    close(wire[0]);
    close(wire[1]);
    close(output[0]);
    close(output[1]);
    run.dev = run.master = -1;
    bridge_clear();
}

int main(void)
{
    lv_init();
    test_csim_bridge();
    assert(!lpac_present());
    program_present = true;
    assert(lpac_present());
    program_present = false;
    assert(!lpac_present());
    lpac_stop();
    backend_present = true;
    assert(!lpac_present());
    program_present = true;
    assert(lpac_present());
    lpac_stop();
    backend_present = false;
    program_present = true;
    const char *replies[] = {"\r\nOK\r\n", "ERROR\r\n", "+CME ERROR: 10\r\n", "", "NOT OK\r\n"};
    for (unsigned i = 0; i < sizeof replies / sizeof *replies; i++) {
        int fds[2];
        assert(pipe(fds) == 0);
        run.master = fds[1];
        on_plain(replies[i]);
        close(fds[1]);
        char answer[16] = {0};
        assert(read(fds[0], answer, sizeof answer - 1) > 0);
        assert(!strcmp(answer, i == 0 ? "OK\r\n" : "ERROR\r\n"));
        close(fds[0]);
    }
    run.master = -1;

    int fds[2];
    assert(pipe(fds) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(fds[0]);
        FILE *out = fdopen(fds[1], "w");
        fputs("{\"type\":\"lpa\",\"payload\":{\"code\":0,\"data\":\"", out);
        for (int i = 0; i < 50000; i++)
            fputc('x', out);
        fputs("\"}}", out);
        fclose(out);
        _exit(0);
    }
    close(fds[1]);
    receive(child, fds[0]);
    assert(result_ok && result_length == 50000);

    int ready[2];
    assert(pipe(fds) == 0 && pipe(ready) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) {
        close(fds[0]);
        close(ready[0]);
        signal(SIGINT, cancel_signal);
        assert(write(ready[1], "r", 1) == 1);
        close(ready[1]);
        while (!interrupted)
            usleep(1000);
        const char text[] = "{\"type\":\"lpa\",\"payload\":{\"code\":-1,\"message\":\"cancelled\"}}\n";
        assert(write(fds[1], text, sizeof text - 1) == sizeof text - 1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    run.pid = child;
    lpac_cancel();
    assert(run.cancelling);
    receive(child, fds[0]);
    assert(!result_ok && !run.cancelling);

    assert(pipe(fds) == 0);
    run.in = fds[1];
    run.preview = true;
    assert(lpac_download_confirm(true) && !run.preview);
    char answer[2];
    assert(read(fds[0], answer, 2) == 1 && answer[0] == 'y');
    close(fds[0]);
    close(fds[1]);
    run.in = -1;

    run.refresh_ok = true;
    on_off("ERROR\r\n");
    on_radio("OK\r\n");
    run.refresh_done = result;
    completed = false;
    on_refresh("OK\r\n");
    assert(completed && !result_ok);
    bridge_clear();

    char activation[4096];
    memset(activation, 'a', sizeof activation - 1);
    activation[sizeof activation - 1] = 0;
    ending = true;
    lpac_set_interface(true);
    char aid[] = "A06573746B6D65FFFF4953442D522031";
    lpac_call(1, aid, (char *const[]){"profile", "download", "-a", activation, NULL}, NULL, result);
    aid[0] = 'B';
    lpac_set_interface(false);
    assert(run.q[run.head].csim);
    assert(!strcmp(run.q[run.head].aid, "A06573746B6D65FFFF4953442D522031"));
    assert(!strcmp(run.q[run.head].argv[4], activation));
    free_job(&run.q[run.head]);
    run.count = 0;
    ending = false;
    run.refreshing = true;
    run.refresh_ok = true;
    run.refresh_done = result;
    completed = false;
    on_estk_channel("2");
    assert(!completed && at.active);
    at.active = false;
    at.then("OK\r\n");
    assert(completed && result_ok && !run.refreshing);
    run.refreshing = true;
    run.refresh_ok = true;
    run.refresh_done = result;
    completed = false;
    on_estk_channel(NULL);
    assert(completed && !result_ok && !run.refreshing);
    assert(setenv("LPAC_CUSTOM_ISD_R_AID", "inherited", 1) == 0);
    completed = false;
    lpac_call(1, "A06573746B6D65FFFF4953442D522031", (char *const[]){"chip", "info", NULL}, NULL, result);
    for (int attempt = 0; !completed && attempt < 1000; attempt++) {
        output_poll();
        usleep(1000);
    }
    assert(completed && !strcmp(result_aid, "A06573746B6D65FFFF4953442D522031"));
    assert(!strcmp(run.transport, "at:curl"));
    completed = false;
    lpac_call(0, NULL, (char *const[]){"chip", "info", NULL}, NULL, result);
    for (int attempt = 0; !completed && attempt < 1000; attempt++) {
        output_poll();
        usleep(1000);
    }
    assert(completed && !strcmp(result_aid, "standard"));
    lpac_stop();
    backend_present = true;
    completed = false;
    lpac_call(1, "A06573746B6D65FFFF4953442D522031", (char *const[]){"chip", "info", NULL}, NULL, result);
    assert(run.master == -1 && run.dev == -1);
    for (int attempt = 0; !completed && attempt < 1000; attempt++) {
        output_poll();
        usleep(1000);
    }
    assert(completed && result_ok && !strcmp(run.transport, "system"));
    lpac_stop();
    puts("LPAC: replies, 50KB output, cancellation, preview, refresh, arguments and per-job SE selection passed");
}
