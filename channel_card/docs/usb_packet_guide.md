# Channel Card streaming and packet guide

This describes the current CDC protocol implemented in `USB_APP/usb_stream.h`,
`USB_APP/usb_app.c` and `voice_bd/voicebd.cpp`. The examples are illustrative,
not a capture from a running Rockchip. For the complete contract, see
[USB protocol](protocol.md#4-usb-protocol-channel-card).

## What changed

| Before | Now |
|---|---|
| USB audio device used to carry our custom data | One USB CDC data port per Channel Card |
| RtAudio supplied the host audio callback | A dedicated worker sends framed serial data |
| Fixed 1008-byte UAC transfer every 1 ms while streaming | Variable-length messages sent when samples or uploads are needed |
| Ten-byte metadata header describing up to two voice blocks per audio transfer | Five-byte header identifying one message and its payload |
| Separate USB upload and audio interfaces | BODY, attack data and scripts share the CDC connection |
| TinyUSB/UAC in Channel firmware | Custom USB CDC implementation over STM32 HAL PCD |

RS485 still carries notes, controls and buffer status. All eight voices still
play concurrently. They share the board's one USB connection; there is not a
separate USB port for each voice. Audio remains signed 8-bit source samples,
with the same 48 kHz DAC output, voice rings, envelope VM and attack/BODY design.

Bulk USB acknowledges/retries transactions, but it does not reserve the old
isochronous 1 ms service interval. The worker sends ready data immediately;
end-to-end latency must be measured on the target hardware.

## One message format

```text
[type:1][target:1][session:1][length:2][payload:length]
|---------- 5-byte header ----------|                |
```

All multi-byte integers are little-endian (least significant byte first).
`length` counts only payload bytes. Maximum payload: 1024 bytes. Maximum complete
message: 1029 bytes. There is no padding, delimiter, or application-level CRC.
The BEC program itself has its own validation header and CRC.

| Byte offset | Size | Meaning |
|---|---:|---|
| 0 | 1 | Message type, listed below |
| 1 | 1 | BODY voice 0–7; upload sample/wavetable/voice target according to upload kind |
| 2 | 1 | BODY note-session ID, 0–254; zero for other host requests. REPLY uses this byte for the original request type. |
| 3–4 | 2 | Payload length in bytes |
| 5 onward | length | Payload |

Why these fields remain:

- **Type** distinguishes samples, upload chunks and handshake/control messages
  sharing the same byte stream.
- **Target** routes BODY to voice 0–7, or an upload to its sample/voice slot.
- **Session** distinguishes successive notes using the same voice slot. Late
  blocks from a retired note are discarded rather than played by its replacement.
- **Length** marks the end of this message even when USB packets or serial reads
  split or combine messages. Two bytes support variable payloads up to 1024 bytes.

No START flag is needed: RS485 arms the pending note session, and its first
matching BODY starts filling the empty pending ring. There is no wire sequence
number; both ends count BODY blocks internally. No request ID is needed because
only one control/upload request may await its reply. On an ambiguous timeout
or disconnect, the connection must reopen before another request is sent.

This is **protocol version 2**. Host and firmware must be updated together;
the old eight-byte protocol is incompatible.

| BODY samples | Header | Total message bytes | Header share of message |
|---:|---:|---:|---:|
| 240 | 5 | 245 | 2.04% |
| 998 (initial priming) | 5 | 1003 | 0.50% |
| 1024 (maximum) | 5 | 1029 | 0.49% |

These figures exclude USB transaction overhead. One signed-int8 source sample
occupies one payload byte. Compared with v1, every message saves three header
bytes; this reduces protocol overhead but is not a measured latency improvement.

## Message types

| Type | Name | Direction | Payload / use |
|---|---|---|---|
| `01` | HELLO | Host → card | One byte: protocol version `02`. Negotiate capabilities once after opening. |
| `02` | BODY | Host → card | 1–1024 signed 8-bit source samples for the header's voice/session. |
| `03` | UPLOAD_BEGIN | Host → card | Upload kind (1 byte), then total size (4 bytes). |
| `04` | UPLOAD_DATA | Host → card | Byte offset (4 bytes), then a data chunk. The current host uses at most 512 data bytes per chunk. |
| `05` | UPLOAD_ABORT | Host → card | Empty payload; cancel an incomplete upload. |
| `06` | REPLY | Card → host | First payload byte: `00` success or `01` error. Remaining data depends on the request. |
| `07` | PROBE | Diagnostic client → card | Firmware-only diagnostic support; the current production host does not send it. |

Upload kinds: `01` = sample attack, `02` = wavetable, `03` = Berry script.
The production host uploads attacks and scripts. ATTACK targets sample slots;
SCRIPT targets voice slots 0–7. Each upload data chunk starts with its absolute
byte offset; the firmware checks offset and length. Completion is automatic
when the declared total size is reached. BODY messages can be interleaved with
an upload; only one upload transaction is active at a time.

A HELLO success reply contains status, protocol version, voice count, sample
width, DAC rate, maximum payload, note priming threshold, and the current
16-bit processed BODY counter. Other successful
upload replies contain only `00`. Errors can append text after status `01`.
REPLY echoes the request target, with the request type in header byte 2.
The host checks reply type, target, status and successful payload length.

## Examples in hex

### Open the connection

HELLO:

```text
01 00 00 01 00 | 02
```

HELLO reply:

```text
06 00 01 0e 00 | 00 02 08 01 80 bb 00 00 00 04 e6 03 00 00
```

The reply advertises success, protocol 2, eight voices, one byte/sample,
48000 Hz, maximum payload 1024, initial BODY priming of 998 samples, and a
processed BODY counter of zero. After reconnect, this counter may be nonzero;
the host uses the value returned by HELLO, not an earlier RS485 snapshot.

### Start a note and deliver BODY

The note command is sent on RS485, not inside a USB message:

```text
c:n3 on 0 60 100 @7\r
```

This selects voice 3, sample 0, MIDI key 60, velocity 100, session 7.
After its RS485 acknowledgement, a possible first USB BODY message is:

```text
02 03 07 e6 03 | [998 sample bytes]
```

This means BODY, voice 3, session 7, length 998. The card recognizes the
first block of the pending session without a START flag. A later refill could be:

```text
02 03 07 f0 00 | [240 sample bytes]
```

That is 240 samples for the same voice/session. At normal source consumption,
240 samples represents 5 ms; pitch/script changes alter consumption. Neither
this length nor a 5 ms send interval is fixed by the protocol. Other voices'
BODY messages can appear between these messages.

### Upload a 512-byte attack to sample slot 0

Begin, kind ATTACK, total size 512:

```text
03 00 00 05 00 | 01 00 02 00 00
```

After the successful begin reply, send data at offset zero:

```text
04 00 00 04 02 | 00 00 00 00 [512 attack bytes]
```

The payload length is 516: four offset bytes plus 512 data bytes. Success reply:

```text
06 00 04 01 00 | 00
```

## What happens while playing

1. Host opens CDC and exchanges HELLO. It uploads Berry programs for voices 0–7.
2. Loading a sound uploads its attack prefix (up to 512 samples). The host keeps
   the PCM source for subsequent BODY streaming.
3. A MIDI note causes an RS485 note-on with a new session. The host then sends
   that voice's initial BODY data. The existing startup gate requires 998 BODY
   samples; the card begins playback at an audio boundary.
4. The USB worker interleaves voice messages, favoring urgent refills and new
   notes. Several messages can be batched in one write. One BODY message carries
   one voice's data; this does not limit the board to one voice per millisecond.
5. Normally every 5 ms, RS485 `c:vq` requests a 61-byte binary status snapshot.
   It supplies the cumulative processed BODY count, per-voice free space and demand.
   Polling can shorten to 1 ms when demand is high. The host retains its bounded
   five-ms forecast and two-ms safety reserve between replies.
6. Note-off goes over RS485. A release envelope can still need BODY data. Once
   all voices are inactive and no upload is pending, the host sends no USB
   application data. RS485 status polling and USB bus housekeeping can continue.

## Acknowledgements and buffer limits

USB hardware ACK/retry operates underneath this protocol. Successful BODY
messages do not receive individual application replies. RS485 `vq` cumulatively
reports the cumulative BODY count modulo 65536; stale-note blocks are included
in that accounting. The firmware increments it once per processed block, never
for a partial frame or repeated attempts to fit a retained block into its ring.
The host counts blocks in the same stream order. HELLO establishes their shared
baseline after each reopen. Upload requests receive individual USB REPLY messages
and do not advance the BODY counter.

The host tracks outstanding BODY messages and subtracts their samples from
reported free space, preventing reuse of the same credit. The outstanding BODY
window is at most 8232 bytes including headers. Each voice ring holds 4080
samples. The firmware retains a complete block that cannot yet fit and applies
USB backpressure. A protocol fault stops the link; note-off remains available
on RS485. This flow control does not guarantee against underrun under every
possible host stall.

## Reading a USB capture

A protocol message is not the same thing as a physical USB packet or a serial
read/write. This Full-Speed device transfers up to 64 bytes per bulk USB packet;
a message can span multiple packets, and a packet can contain the end of one
message and the start of another. Reassemble the CDC byte stream, read five
header bytes, then consume exactly the declared payload length before decoding
the next header. Sample bytes can equal a type code; do not scan the payload
looking for headers. A valid capture should start at connection setup.

The header tells you whether the bytes are audio BODY, attack/script upload,
handshake, or reply, and which voice/session or upload target they belong to.
The `c:usb` diagnostics are aggregate counters, not a list of individual packets.
