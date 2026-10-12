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

#include "backend_model.h"
#include "audio.h"
#ifdef LEGACY_API
#include "board.h"
#else
#include "platform.h"
#endif
#include "channel.h"
#include "usb.h"
#ifndef LEGACY_API
#include "transport.h"
#include "audio_output.h"
#endif
#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <string.h>
namespace model {
uint32_t time_ms, epoch, read_limit=64, tx_capacity=2048, drops, tx_polls;
bool connected=true, faulted, tx_stalled, bus_owned;
unsigned usb_services, aborts;
std::vector<uint8_t> usb_rx, usb_tx, rs_rx, rs_tx;
static std::vector<uint8_t> pending;
void reconnect() { ++epoch; connected=true; faulted=false; usb_rx.clear(); usb_tx.clear(); }
}
using namespace model;
uint32_t get_board_time_ms() { return time_ms++; }
#ifdef LEGACY_API
uint32_t disable_interrupts_and_save_state() { return 0; }
void restore_interrupt_state(uint32_t) {}
void write_board_led(uint8_t, uint8_t) {}
void write_board_switch(uint8_t, uint8_t) {}
void release_rs485_bus() { bus_owned=false; }
void acquire_rs485_bus() { assert(!bus_owned); bus_owned=true; }
uint8_t is_rs485_bus_free() { return !bus_owned; }
uint32_t get_rs485_baud_rate() { return 921600; }
uint8_t is_dac_ready() { return 1; }
void set_dac_channel_trim(char, uint8_t) {}
#else
static void release_bus() { bus_owned=false; }
static void acquire_bus() { assert(!bus_owned); bus_owned=true; }
static uint8_t bus_free() { return !bus_owned; }
static uint32_t baud_rate() { return 921600; }
#endif
void init_rs485_receiver() { rs_rx.clear(); drops=0; }
void clear_rs485_receive_state() { rs_rx.clear(); }
uint8_t read_rs485_byte(uint8_t *out) {
    if (rs_rx.empty()) return 0;
    *out=rs_rx.front(); rs_rx.erase(rs_rx.begin()); return 1;
}
uint32_t get_rs485_dropped_byte_count() { return drops; }
#ifdef LEGACY_API
int write_rs485_bytes(const uint8_t *p, uint16_t n, uint32_t) {
    rs_tx.insert(rs_tx.end(),p,p+n); return 0;
}
#else
int start_rs485_transmit(const uint8_t *p, uint16_t n) {
    if (!p || !n || !pending.empty()) return -1;
    pending.assign(p,p+n); return 0;
}
int poll_rs485_transmit() {
    if (tx_stalled) return 0;
    if (tx_polls) { --tx_polls; return 0; }
    rs_tx.insert(rs_tx.end(),pending.begin(),pending.end()); pending.clear(); return 1;
}
void abort_rs485_transmit() { ++aborts; pending.clear(); }
#endif
void init_usb_device() {
#ifndef LEGACY_API
    static const Rs485BusServices bus = {acquire_bus, release_bus, bus_free, baud_rate};
    set_rs485_bus_services(&bus);
#endif
    reconnect();
}
uint32_t read_usb_bytes(uint8_t *out,uint32_t n) {
    ++usb_services;
    if (faulted || !connected) return 0;
    n=std::min(n,std::min(read_limit,(uint32_t)usb_rx.size()));
    if(n) memcpy(out,usb_rx.data(),n);
    usb_rx.erase(usb_rx.begin(),usb_rx.begin()+n); return n;
}
uint8_t write_usb_bytes(const uint8_t *p,uint32_t n) {
    if(!connected || n>tx_capacity-usb_tx.size()) return 0;
    usb_tx.insert(usb_tx.end(),p,p+n); return 1;
}
uint32_t get_usb_transmit_free_bytes() { return tx_capacity-usb_tx.size(); }
uint32_t get_usb_connection_generation() { return epoch; }
uint8_t is_usb_connected() { return connected && !faulted; }
void fault_usb_connection() { faulted=true; }
void format_usb_device_status(char *p,uint32_t n) { snprintf(p,n,"ok: model\r\n"); }
