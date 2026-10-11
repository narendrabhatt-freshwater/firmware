#ifndef CHANNEL_SIM_LIBSERIALPORT_H
#define CHANNEL_SIM_LIBSERIALPORT_H

/* Stand-in for <libserialport.h> used only when building the host
 * application against the simulator. It declares the subset voicebd.cpp
 * calls; serialport.cpp implements it over Unix sockets, where the "port
 * name" is the socket path the simulator listens on. The real library cannot
 * open a pseudo-terminal on macOS, so the host cannot reach a simulator
 * through it. voicebd.cpp itself is compiled unchanged. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sp_return { SP_OK = 0, SP_ERR_ARG = -1, SP_ERR_FAIL = -2, SP_ERR_MEM = -3, SP_ERR_SUPP = -4 };
enum sp_mode { SP_MODE_READ = 1, SP_MODE_WRITE = 2, SP_MODE_READ_WRITE = 3 };
enum sp_buffer { SP_BUF_INPUT = 1, SP_BUF_OUTPUT = 2, SP_BUF_BOTH = 3 };
enum sp_parity { SP_PARITY_NONE = 0 };
enum sp_dtr { SP_DTR_OFF = 0, SP_DTR_ON = 1 };
enum sp_flowcontrol { SP_FLOWCONTROL_NONE = 0 };

struct sp_port;

enum sp_return sp_get_port_by_name(const char *name, struct sp_port **port);
void sp_free_port(struct sp_port *port);
enum sp_return sp_open(struct sp_port *port, enum sp_mode mode);
enum sp_return sp_close(struct sp_port *port);
enum sp_return sp_get_port_handle(const struct sp_port *port, void *result);

/* Line settings have no meaning on a socket and always succeed. Opening the
 * port is what the simulator treats as raising DTR. */
enum sp_return sp_set_baudrate(struct sp_port *port, int baudrate);
enum sp_return sp_set_bits(struct sp_port *port, int bits);
enum sp_return sp_set_parity(struct sp_port *port, enum sp_parity parity);
enum sp_return sp_set_stopbits(struct sp_port *port, int stopbits);
enum sp_return sp_set_flowcontrol(struct sp_port *port, enum sp_flowcontrol flowcontrol);
enum sp_return sp_set_dtr(struct sp_port *port, enum sp_dtr dtr);

enum sp_return sp_flush(struct sp_port *port, enum sp_buffer buffers);
enum sp_return sp_blocking_write(struct sp_port *port, const void *buf, size_t count,
                                 unsigned int timeout_ms);
enum sp_return sp_blocking_read_next(struct sp_port *port, void *buf, size_t count,
                                     unsigned int timeout_ms);
enum sp_return sp_nonblocking_read(struct sp_port *port, void *buf, size_t count);
enum sp_return sp_nonblocking_write(struct sp_port *port, const void *buf, size_t count);

char *sp_last_error_message(void);
void sp_free_error_message(char *message);

#if defined(__APPLE__)
/* voicebd.cpp sets the macOS serial receive latency with a raw ioctl, which a
 * socket rejects. Route its ioctl calls here: that one request succeeds as a
 * no-op and every other request reaches the real ioctl. */
int sim_ioctl(int fd, unsigned long request, ...);
#define ioctl sim_ioctl
#endif

#ifdef __cplusplus
}
#endif

#endif
