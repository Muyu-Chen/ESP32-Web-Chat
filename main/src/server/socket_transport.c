#include "server/socket_transport.h"

#include <errno.h>
#include <limits.h>

#include "esp_timer.h"
#include "lwip/sockets.h"

#include "chat_config.h"

static int send_complete(httpd_handle_t server, int fd, const char *buffer, size_t length, int flags)
{
    (void)server;
    if ((buffer == NULL && length != 0) || length > INT_MAX) {
        return HTTPD_SOCK_ERR_INVALID;
    }

    size_t sent = 0;
    int failure = EPIPE;
    int64_t started_at = esp_timer_get_time();
    while (sent < length) {
        if (esp_timer_get_time() - started_at >= HTTP_SEND_TIMEOUT_S * 1000000LL) {
            failure = EAGAIN;
            break;
        }

        ssize_t result = send(fd, buffer + sent, length - sent, flags);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            failure = result < 0 ? errno : EPIPE;
            break;
        }
        sent += (size_t)result;
    }

    if (sent == length) {
        return (int)sent;
    }

    // A WS frame header may already be on the wire; never append another frame after failure.
    shutdown(fd, SHUT_RDWR);
    errno = failure;
    return failure == EAGAIN || failure == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
}

esp_err_t chat_http_session_open_handler(httpd_handle_t server, int fd)
{
    return httpd_sess_set_send_override(server, fd, send_complete);
}
