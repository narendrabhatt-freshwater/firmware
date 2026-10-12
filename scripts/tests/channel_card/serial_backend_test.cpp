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

/* Exercise the real PTY backend's queues and lifecycle with deterministic
 * short writes/errors. Only this test translation unit intercepts write(). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
static size_t write_limit=1;
static int write_error;
static ssize_t limited_write(int fd,const void *p,size_t n)
{
    if(write_error) { errno=write_error; return -1; }
    return ::write(fd,p,n<write_limit?n:write_limit);
}
#define write limited_write
#include "serialdevices.cpp"
#undef write
uint64_t get_simulator_time_ns()
{
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void pair_for(Port &p,int &peer)
{
    int fds[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
    p.fd=fds[0]; peer=fds[1];
    assert(fcntl(p.fd,F_SETFL,O_NONBLOCK)==0);
    assert(fcntl(peer,F_SETFL,O_NONBLOCK)==0);
    p.connected=true;
}
int main()
{
    int usb_peer,rs_peer; pair_for(usb,usb_peer); pair_for(rs485,rs_peer);
    uint8_t data[]={1,2,3,4}, received[8]={};
    assert(start_rs485_transmit(data,4)==0);
    assert(start_rs485_transmit(data,4)==-1);
    assert(poll_rs485_transmit()==0 && rs485.sent==3);
    write_error=EAGAIN; assert(poll_rs485_transmit()==0 && rs485.sent==3);
    write_error=EINTR; assert(poll_rs485_transmit()==0 && rs485.sent==3);
    write_error=0;
    assert(poll_rs485_transmit()==0); assert(poll_rs485_transmit()==0);
    assert(poll_rs485_transmit()==1);
    assert(read(rs_peer,received,8)==4 && memcmp(received,data,4)==0);
    assert(start_rs485_transmit(data,4)==0); write_error=EAGAIN;
    assert(poll_rs485_transmit()==0); abort_rs485_transmit(); assert(rs485.sent==0);
    write_error=0;
    // USB application writes stay all-or-nothing despite partial OS writes.
    assert(write_usb_bytes(data,4)); usb.sent=sizeof usb.tx-1;
    assert(!write_usb_bytes(data,4) && usb.sent==sizeof usb.tx-1);
    usb.sent=4;
    for(unsigned i=0;i<4;++i) assert(transmit_serial_bytes(usb));
    assert(read(usb_peer,received,8)==4 && memcmp(received,data,4)==0);
    // Packet-mode payloads may be consumed in arbitrary prefixes.
    uint8_t packet[]={0,7,8,9};
    assert(::write(usb_peer,packet,4)==4); assert(receive_serial_bytes(usb));
    assert(read_usb_bytes(received,2)==2 && received[0]==7 && received[1]==8);
    assert(read_usb_bytes(received,8)==1 && received[0]==9);
    fault_usb_connection(); assert(!is_usb_connected());
    assert(read_usb_bytes(received,8)==0);
    uint32_t generation=get_usb_connection_generation();
    packet[0]=TIOCPKT_FLUSHREAD;
    assert(::write(usb_peer,packet,1)==1); assert(receive_serial_bytes(usb));
    assert(get_usb_connection_generation()!=generation && is_usb_connected());
    // Disconnect while a transmission is pending is failure, not completion.
    assert(start_rs485_transmit(data,4)==0); close(rs_peer);
    assert(poll_rs485_transmit()==-1); abort_rs485_transmit();
    assert(start_rs485_transmit(data,4)==0); assert(poll_rs485_transmit()==1);
    close(usb_peer); close(usb.fd); close(rs485.fd);
    return 0;
}
