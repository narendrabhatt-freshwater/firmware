/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#include "serialdevices.h"
#include "transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* ---- serial port state -------------------------------------------------- */

struct Port {
    int fd = -1;
    const char *path = nullptr;
    std::string slave;
    bool connected = false, linked = false;
    uint8_t rx[8192], tx[2048];
    size_t received = 0, sent = 0;
};

static Port usb, rs485;
static uint32_t usbEpoch;
static bool usbFault;
static bool readProgress;
static bool rsTxPending, rsTxFailed, serialIoFailed;

/* ---- open and close serial devices -------------------------------------- */

static void reset_serial_port(Port &port, bool connected)
{
    if (&port == &rs485 && rsTxPending) rsTxFailed = true;
    port.connected = connected;
    port.received = port.sent = 0;
    if (&port == &usb) {
        ++usbEpoch;
        usbFault = false;
    }
}

static bool open_serial_port(Port &port, const char *path)
{
    port.path = path;
    struct stat info;
    if (lstat(path, &info) == 0) {
        if (!S_ISLNK(info.st_mode) || stat(path, &info) == 0 || errno != ENOENT) {
            errno = EEXIST;
            return false;
        }
        if (unlink(path) < 0) return false;
    } else if (errno != ENOENT) {
        return false;
    }
    port.fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port.fd < 0 || grantpt(port.fd) < 0 || unlockpt(port.fd) < 0) return false;
    const char *name = ptsname(port.fd);
    if (!name) return false;
    port.slave = name;
    int slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (slave < 0) return false;
    termios mode;
    bool ready = tcgetattr(slave, &mode) == 0;
    if (ready) {
        cfmakeraw(&mode);
        mode.c_cflag |= CLOCAL | CREAD;
        ready = tcsetattr(slave, TCSANOW, &mode) == 0;
    }
    int saved = errno;
    /* Keeping this open would hide host disconnections. */
    close(slave);
    if (!ready) {
        errno = saved;
        return false;
    }
    int packet = 1;
    if (ioctl(port.fd, TIOCPKT, &packet) < 0 || symlink(name, path) < 0) return false;
    port.linked = true;
    return true;
}

bool setup_serial_devices(const char *usb_path, const char *rs485_path)
{
    if (open_serial_port(usb, usb_path) && open_serial_port(rs485, rs485_path)) return true;
    perror("sim: creating serial devices");
    close_serial_devices();
    return false;
}

void close_serial_devices(void)
{
    for (Port *port : {&usb, &rs485}) {
        char target[PATH_MAX];
        ssize_t n = port->linked ? readlink(port->path, target, sizeof target) : -1;
        if (n >= 0 && port->slave == std::string(target, (size_t)n)) unlink(port->path);
        if (port->fd >= 0) close(port->fd);
        port->fd = -1;
        port->linked = false;
    }
}

/* ---- receive and transmit pty data -------------------------------------- */

static bool is_temporary_io_error(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static bool receive_serial_bytes(Port &port)
{
    pollfd state{port.fd, 0, 0};
    if (poll(&state, 1, 0) > 0 && (state.revents & POLLHUP)) {
        if (port.connected) reset_serial_port(port, false);
        return true;
    }
    uint8_t packet[sizeof port.rx + 1];
    size_t space = sizeof port.rx - port.received;
    if (&port == &usb && usbFault) space = 0;
    /* Read the packet header even when full: a host flush resets the session. */
    ssize_t n = read(port.fd, packet, space + 1);
    if (n < 0 && is_temporary_io_error()) return true;
    if (n == 0 || (n < 0 && errno == EIO)) {
        if (port.connected) reset_serial_port(port, false);
        return true;
    }
    if (n < 0) {
        perror("sim: reading PTY");
        return false;
    }
    if (packet[0] & (TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE)) reset_serial_port(port, false);
    if (!port.connected) reset_serial_port(port, true);
    if (packet[0] == TIOCPKT_DATA && n > 1) {
        memcpy(port.rx + port.received, packet + 1, (size_t)n - 1);
        port.received += (size_t)n - 1;
        fprintf(stderr, "sim: received %zu bytes from %s\n", n - 1, port.path);
    }
    return true;
}

static bool transmit_serial_bytes(Port &port)
{
    if (!port.connected || !port.sent) return true;
    ssize_t n = write(port.fd, port.tx, port.sent);
    if (n < 0 && is_temporary_io_error()) return true;
    if (n < 0 && errno == EIO) {
        reset_serial_port(port, false);
        return true;
    }
    if (n <= 0) {
        perror("sim: writing PTY");
        return false;
    }
    port.sent -= (size_t)n;
    memmove(port.tx, port.tx + n, port.sent);
    return true;
}

/* ---- service serial devices --------------------------------------------- */

bool process_serial_io(uint64_t deadlineNs)
{
    if (serialIoFailed) return false;
    fd_set readable, writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    int top = -1;
    uint64_t now = get_simulator_time_ns();
    uint64_t delay = deadlineNs > now ? deadlineNs - now : 0;
    for (Port *port : {&usb, &rs485}) {
        /* Closed PTYs stay readable at EOF; check them on the audio tick. */
        if (!port->connected) continue;
        bool canRead = port != &usb || !usbFault;
        if (canRead && port->received < sizeof port->rx) FD_SET(port->fd, &readable);
        if (port->sent) FD_SET(port->fd, &writable);
        if (canRead && port->received && readProgress) delay = 0;
        if (port->fd > top) top = port->fd;
    }
    readProgress = false;
    timespec timeout{(time_t)(delay / 1000000000), (long)(delay % 1000000000)};
    if (pselect(top + 1, &readable, &writable, nullptr, &timeout, nullptr) < 0 && errno != EINTR) {
        perror("sim: waiting for PTYs");
        serialIoFailed = true;
        return false;
    }
    for (Port *port : {&usb, &rs485}) {
        if (!receive_serial_bytes(*port) || !transmit_serial_bytes(*port)) {
            serialIoFailed = true;
            return false;
        }
    }
    return true;
}

/* ---- read and write buffers --------------------------------------------- */

static uint32_t read_port_buffer(Port &port, uint8_t *out, uint32_t size)
{
    if (size > port.received) size = (uint32_t)port.received;
    if (size) readProgress = true;
    memcpy(out, port.rx, size);
    port.received -= size;
    memmove(port.rx, port.rx + size, port.received);
    return size;
}

static bool write_port_buffer(Port &port, const uint8_t *data, uint32_t size)
{
    if (!port.connected || size > sizeof port.tx - port.sent) return false;
    memcpy(port.tx + port.sent, data, size);
    port.sent += size;
    return true;
}

/* ---- usb device interface ----------------------------------------------- */

void init_usb_device(void)
{
    reset_serial_port(usb, false);
}

uint32_t read_usb_bytes(uint8_t *out, uint32_t size)
{
    return usbFault ? 0 : read_port_buffer(usb, out, size);
}

uint8_t write_usb_bytes(const uint8_t *data, uint32_t size)
{
    return write_port_buffer(usb, data, size);
}

uint32_t get_usb_transmit_free_bytes(void)
{
    return (uint32_t)(sizeof usb.tx - usb.sent);
}

uint32_t get_usb_connection_generation(void)
{
    return usbEpoch;
}

uint8_t is_usb_connected(void)
{
    return usb.connected && !usbFault;
}

void fault_usb_connection(void)
{
    usbFault = true;
}

void format_usb_device_status(char *text, uint32_t size)
{
    snprintf(text, size, "ok: usbdev PTY attached %u fault %u epoch %lu\r\n",
             usb.connected, usbFault, (unsigned long)usbEpoch);
}

/* ---- rs485 device interface --------------------------------------------- */

void init_rs485_receiver(void)
{
    rs485.received = 0;
}

void clear_rs485_receive_state(void)
{
    rs485.received = 0;
}

uint8_t read_rs485_byte(uint8_t *out)
{
    return (uint8_t)read_port_buffer(rs485, out, 1);
}

uint32_t get_rs485_dropped_byte_count(void)
{
    return 0;
}

int start_rs485_transmit(const uint8_t *data, uint16_t size)
{
    if (!data || !size || rsTxPending) return -1;
    rsTxFailed = false;
    /* RS485 has no peer-presence acknowledgement. A boot transmission with
     * no PTY listener has no recipient, just as on the physical bus. */
    if (!rs485.connected) return 0;
    if (!write_port_buffer(rs485, data, size)) return -1;
    rsTxPending = true;
    return 0;
}

int poll_rs485_transmit(void)
{
    if (!rsTxPending) return 1;
    /* Nonblocking device service only; shared code owns protocol dispatch. */
    if (!process_serial_io(get_simulator_time_ns()) || rsTxFailed) return -1;
    if (rs485.sent) return 0;
    rsTxPending = false;
    return 1;
}

void abort_rs485_transmit(void)
{
    rs485.sent = 0;
    rsTxPending = rsTxFailed = false;
}
