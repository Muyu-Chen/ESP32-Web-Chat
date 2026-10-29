#include "support.h"

#include <errno.h>
#include <sys/socket.h>

static ssize_t mock_send(int fd, const void *buffer, size_t length, int flags);
static int mock_shutdown(int fd, int how);

// Exercise the production transport with deterministic short writes and failures.
#define send mock_send
#define shutdown mock_shutdown
#include "../../main/src/server/socket_transport.c"
#undef send
#undef shutdown

static httpd_send_func_t send_override;
static int results[8];
static size_t result_count, result_index, written;
static char wire[32];
static int shutdown_count;
static bool advance_clock;

esp_err_t httpd_sess_set_send_override(httpd_handle_t server, int fd, httpd_send_func_t callback)
{
    assert(server == &ctx && fd == 10);
    send_override = callback;
    return ESP_OK;
}

static ssize_t mock_send(int fd, const void *buffer, size_t length, int flags)
{
    assert(fd == 10 && flags == 0 && result_index < result_count);
    int result = results[result_index++];
    if (result < 0) {
        errno = -result;
        return -1;
    }
    assert((size_t)result <= length && written + result <= sizeof(wire));
    memcpy(wire + written, buffer, result);
    written += result;
    if (advance_clock) { now_s += HTTP_SEND_TIMEOUT_S; }
    return result;
}

static int mock_shutdown(int fd, int how)
{
    assert(fd == 10 && how == SHUT_RDWR);
    shutdown_count++;
    errno = ENOTCONN;
    return 0;
}

static void configure(const int *pattern, size_t count)
{
    reset();
    memcpy(results, pattern, count * sizeof(*pattern));
    result_count = count;
    result_index = written = 0;
    shutdown_count = 0;
    advance_clock = false;
    assert(chat_http_session_open_handler(&ctx, 10) == ESP_OK);
}

int main(void)
{
    const int short_writes[] = { 1, 2, 3 };
    configure(short_writes, 3);
    assert(send_override(&ctx, 10, "abcdef", 6, 0) == 6);
    assert(written == 6 && memcmp(wire, "abcdef", 6) == 0 && shutdown_count == 0);

    const int interrupted[] = { -EINTR, 6 };
    configure(interrupted, 2);
    assert(send_override(&ctx, 10, "abcdef", 6, 0) == 6 && shutdown_count == 0);

    const int timed_out[] = { 2, -EAGAIN };
    configure(timed_out, 2);
    assert(send_override(&ctx, 10, "abcdef", 6, 0) == HTTPD_SOCK_ERR_TIMEOUT);
    assert(shutdown_count == 1 && written == 2 && errno == EAGAIN);

    const int stopped[] = { 0 };
    configure(stopped, 1);
    assert(send_override(&ctx, 10, "abcdef", 6, 0) == HTTPD_SOCK_ERR_FAIL);
    assert(shutdown_count == 1 && errno == EPIPE);

    configure(short_writes, 3);
    advance_clock = true;
    assert(send_override(&ctx, 10, "abcdef", 6, 0) == HTTPD_SOCK_ERR_TIMEOUT);
    assert(result_index == 1 && shutdown_count == 1);
    assert(send_override(&ctx, 10, NULL, 0, 0) == 0);
    assert(send_override(&ctx, 10, NULL, 1, 0) == HTTPD_SOCK_ERR_INVALID);
    reset();
    puts("Transport regressions passed: short writes, EINTR, timeout, zero progress and retry budget");
}
