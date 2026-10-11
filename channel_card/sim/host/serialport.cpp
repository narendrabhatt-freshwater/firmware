/* Unix-socket implementation of the libserialport subset in libserialport.h. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <IOKit/serial/ioss.h>
#endif

/* Declare the real ioctl before the header renames it. */
#include "libserialport.h"
#undef ioctl

struct sp_port {
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int fd;
};

static int s_errno;

/* ---- open and close a port ---------------------------------------------- */

enum sp_return sp_get_port_by_name(const char *name, struct sp_port **port)
{
    if (name == NULL || port == NULL || strlen(name) >= sizeof((*port)->path)) return SP_ERR_ARG;
    struct sp_port *p = (struct sp_port *)calloc(1, sizeof *p);
    if (p == NULL) return SP_ERR_MEM;
    strcpy(p->path, name);
    p->fd = -1;
    *port = p;
    return SP_OK;
}

void sp_free_port(struct sp_port *port)
{
    free(port);
}

enum sp_return sp_open(struct sp_port *port, enum sp_mode mode)
{
    (void)mode;
    if (port == NULL || port->fd >= 0) return SP_ERR_ARG;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        s_errno = errno;
        return SP_ERR_FAIL;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, port->path);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        s_errno = errno;
        close(fd);
        return SP_ERR_FAIL;
    }
#if defined(SO_NOSIGPIPE)
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    port->fd = fd;
    return SP_OK;
}

enum sp_return sp_close(struct sp_port *port)
{
    if (port == NULL || port->fd < 0) return SP_ERR_ARG;
    close(port->fd);
    port->fd = -1;
    return SP_OK;
}

enum sp_return sp_get_port_handle(const struct sp_port *port, void *result)
{
    if (port == NULL || result == NULL) return SP_ERR_ARG;
    *(int *)result = port->fd;
    return SP_OK;
}

/* ---- accept line settings ----------------------------------------------- */

enum sp_return sp_set_baudrate(struct sp_port *port, int baudrate)
{
    (void)baudrate;
    return port != NULL ? SP_OK : SP_ERR_ARG;
}

enum sp_return sp_set_bits(struct sp_port *port, int bits)
{
    (void)bits;
    return port != NULL ? SP_OK : SP_ERR_ARG;
}

enum sp_return sp_set_parity(struct sp_port *port, enum sp_parity parity)
{
    (void)parity;
    return port != NULL ? SP_OK : SP_ERR_ARG;
}

enum sp_return sp_set_stopbits(struct sp_port *port, int stopbits)
{
    (void)stopbits;
    return port != NULL ? SP_OK : SP_ERR_ARG;
}

enum sp_return sp_set_flowcontrol(struct sp_port *port, enum sp_flowcontrol flowcontrol)
{
    (void)flowcontrol;
    return port != NULL ? SP_OK : SP_ERR_ARG;
}

enum sp_return sp_set_dtr(struct sp_port *port, enum sp_dtr dtr)
{
    (void)dtr;
    return port != NULL ? SP_OK : SP_ERR_ARG;
}

/* ---- transfer bytes ----------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

enum sp_return sp_nonblocking_read(struct sp_port *port, void *buf, size_t count)
{
    if (port == NULL || port->fd < 0) return SP_ERR_ARG;
    ssize_t n = recv(port->fd, buf, count, 0);
    if (n > 0) return (enum sp_return)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return SP_OK;
    s_errno = n == 0 ? ECONNRESET : errno; /* the simulator closed the port */
    return SP_ERR_FAIL;
}

enum sp_return sp_nonblocking_write(struct sp_port *port, const void *buf, size_t count)
{
    if (port == NULL || port->fd < 0) return SP_ERR_ARG;
    ssize_t n = send(port->fd, buf, count, SEND_FLAGS);
    if (n >= 0) return (enum sp_return)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return SP_OK;
    s_errno = errno;
    return SP_ERR_FAIL;
}

enum sp_return sp_flush(struct sp_port *port, enum sp_buffer buffers)
{
    if (port == NULL || port->fd < 0) return SP_ERR_ARG;
    if (buffers & SP_BUF_INPUT) {
        char discard[256];
        while (recv(port->fd, discard, sizeof discard, 0) > 0) {
        }
    }
    return SP_OK;
}

/* Returns the bytes written, which is short only if the timeout expired. */
enum sp_return sp_blocking_write(struct sp_port *port, const void *buf, size_t count,
                                 unsigned int timeout_ms)
{
    if (port == NULL || port->fd < 0) return SP_ERR_ARG;
    int64_t deadline = now_ms() + timeout_ms;
    size_t sent = 0;
    while (sent < count) {
        enum sp_return n = sp_nonblocking_write(port, (const char *)buf + sent, count - sent);
        if (n < 0) return n;
        sent += (size_t)n;
        if (sent == count) break;
        int64_t left = deadline - now_ms();
        if (timeout_ms != 0u && left <= 0) break;
        struct pollfd p = {port->fd, POLLOUT, 0};
        (void)poll(&p, 1, timeout_ms == 0u ? -1 : (int)left);
    }
    return (enum sp_return)sent;
}

/* Waits for at least one byte, then returns what is available. */
enum sp_return sp_blocking_read_next(struct sp_port *port, void *buf, size_t count,
                                     unsigned int timeout_ms)
{
    if (port == NULL || port->fd < 0) return SP_ERR_ARG;
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        enum sp_return n = sp_nonblocking_read(port, buf, count);
        if (n != SP_OK) return n;
        int64_t left = deadline - now_ms();
        if (timeout_ms != 0u && left <= 0) return SP_OK;
        struct pollfd p = {port->fd, POLLIN, 0};
        (void)poll(&p, 1, timeout_ms == 0u ? -1 : (int)left);
    }
}

/* ---- report errors ------------------------------------------------------ */

char *sp_last_error_message(void)
{
    return strdup(strerror(s_errno));
}

void sp_free_error_message(char *message)
{
    free(message);
}

/* ---- accept the macos latency request ----------------------------------- */

#if defined(__APPLE__)
int sim_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);
    if (request == IOSSDATALAT) return 0;
    return ioctl(fd, request, argument);
}
#endif
