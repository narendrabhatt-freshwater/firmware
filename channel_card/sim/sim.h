#ifndef CHANNEL_SIM_H
#define CHANNEL_SIM_H

#include <stddef.h>
#include <stdint.h>

/* Host model of the Channel Card around the shared application code.
 * The three event sources the firmware has as interrupts are the three entry
 * points here: USB bytes from the host, RS485 bytes from the host, and the
 * audio clock asking for frames. Everything runs on the caller's thread; an
 * "interrupt" never preempts the main-loop pass, it runs between passes.
 * Time is the audio clock: one millisecond per rendered refill. */

/* ---- start the card ----------------------------------------------------- */

/* Power-on: same initialization order as main(). Call once per process. */
void Sim_Boot(void);

/* ---- usb cdc port ------------------------------------------------------- */

/* Host opens the port and raises DTR / closes it. Both reset the transport. */
void Sim_UsbAttach(void);
void Sim_UsbDetach(void);

/* Bytes the device accepts now. Zero means the OUT endpoint is NAKing. */
size_t Sim_UsbHostSpace(void);
/* Deliver host bytes; returns the count accepted (at most the space above). */
size_t Sim_UsbHostWrite(const uint8_t *data, size_t size);
/* Collect bytes the device queued for the host. */
size_t Sim_UsbHostRead(uint8_t *data, size_t size);

/* ---- rs485 port --------------------------------------------------------- */

void Sim_Rs485HostWrite(const uint8_t *data, size_t size);
size_t Sim_Rs485HostRead(uint8_t *data, size_t size);

/* ---- audio clock -------------------------------------------------------- */

#define SIM_AUDIO_CHANNELS 4u

/* The DAC takes any number of frames, interleaved CH1..CH4, from the card's
 * circular DMA buffers. Each time it leaves half a buffer (every
 * AUDIO_REFILL_FRAMES) the refill interrupt runs, after the main loop has had
 * its turn. `out` may be NULL to discard the samples. */
void Sim_AudioRequest(int32_t *out, uint32_t frames);

/* ---- main loop ---------------------------------------------------------- */

/* Run main-loop passes until one makes no progress, then return: this is
 * where the processor would sleep until the next interrupt. Returns the
 * number of passes that did work. */
uint32_t Sim_RunMainLoop(void);

/* ---- inspect the card --------------------------------------------------- */

uint32_t Sim_Millis(void);
uint8_t Sim_Led(uint8_t led);
uint8_t Sim_Switch(uint8_t sw);
/* Last trim written for DAC channel 1..4, or -1 if never written. */
int Sim_DacTrim(uint8_t channel);
/* Replies sent while the RS485 driver was not enabled; must stay zero. */
uint32_t Sim_Rs485BusErrors(void);

typedef struct {
    uint64_t refills;         /* audio interrupts */
    uint64_t usb_events;      /* host deliveries that carried bytes */
    uint64_t rs485_events;
    uint64_t main_passes;     /* passes that did work */
    uint64_t idle_passes;     /* passes that found nothing: the sleep point */
} SimCounters;

const SimCounters *Sim_Counters(void);

#endif
