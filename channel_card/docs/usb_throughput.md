# Protocol-v3 USB throughput investigation — 18 September 2026

Target: at least **998,000 useful bytes/s**, matching the old UAC layout's
998 sample bytes per 1 ms interval. A bulk throughput average alone does not
establish the old isochronous scheduling or note-to-DAC latency guarantee.

## Continuous CDC measurements

Board: `CHCARD-001B00253034510A31373233` (DFU `315232583034`).
Host: macOS, AppleT8132 USB controller, through a TerraMaster USB 2.0 hub and
a Generic USB2.1 hub. A direct connection was unavailable.

Each run sends 4 MiB of 1024-byte diagnostic PROBE payloads without intermediate
replies. One final ordered barrier verifies the board's received byte count,
block count and FNV-1a hash. Timing includes that final reply. Three runs per
write size; decimal kB/s is numerically equal to bytes/ms. The data pattern is
`00..ff` repeated. No notes or RS485 polling are started by this benchmark.

| Host write size (messages) | Before, median kB/s | Candidate, median kB/s |
|---:|---:|---:|
| 8 | 948.9 | 948.6 |
| 32 | 946.7 | 946.8 |
| 128 | 946.8 | 942.0 |

All nine runs in each set verified successfully. **The candidate has not met
the 998 kB/s target or demonstrated a throughput improvement on this path.**
These are diagnostic receive measurements, not audio playback qualification.

Candidate Release: `channel_MCU_20260918_124046.bin`, 171164 bytes,
SHA-256 `cc6d6e308d02bdba941eff9b0340caacf4834d454e108babb2459fa1e4d13380`.
The full candidate binary was verified by DFU read-back. A full 1 MiB backup
of the original board flash is saved at
`/tmp/channel-cdc-optimization/board-before.bin` for this local session.

## Candidate receive changes

The endpoint still uses 64-byte USB packets. HAL arms a 512-byte transfer to
reduce transfer-completion/rearm work. With USB DMA disabled, HAL publishes its
received count after draining each packet from the FIFO. Main-loop reads copy
that received prefix into the queue without waiting for the entire transfer,
including when the host stops after an exact multiple of 64 bytes. Transfer
completion copies only the remaining suffix. DTR changes discard old prefixes
without resetting USB packet toggles. Queue space is reserved for an entire
receive transfer before rearming.

The parser reads directly into its frame buffer and validates complete headers
and payload spans, removing its temporary buffer and per-byte dispatch loop.

Native regression tests and address/undefined-behavior sanitizer checks pass.
Coverage includes partial transfers without completion, DTR changes while a
transfer is in flight, queued bursts across packet/transfer/parser/ring
boundaries, data hashes, retained BODY backpressure, and reconnect recovery.
STM32 Debug and Release builds pass.

## Host sizing checks

Two-MiB-class transfers with 64 messages per write produced:

| Payload bytes/message | Pattern | Payload kB/s |
|---:|---|---:|
| 1024 | incrementing | 947.2 |
| 1019 (1024-byte framed message) | incrementing | 947.3 |
| 1024 | zero | 947.3 |
| 1019 | zero | 947.3 |
| 507 (512-byte framed message) | zero | 942.7 |

All counts and hashes passed. Aligning framed messages and changing the bit
pattern did not recover the missing bandwidth. This does not establish the
remaining bottleneck: host driver/controller/hub scheduling still needs to be
distinguished from device processing. A direct libusb test requires macOS
administrator access to temporarily detach the CDC driver.

The first direct-libusb attempt (one 4,214,784-byte OUT request with no IN
request pending during the data stream) timed out after reporting 3,166,208
transferred bytes. This is a failed diagnostic, not a throughput result. After
the driver reattached, `usb` showed one protocol error but no drop/hold/full/late
increments; its cause was not identified. A fresh 1 MiB continuous CDC run
verified successfully at 948.3 kB/s (`cdc-after-direct-timeout.json`).
The revised direct test uses 32 KiB host requests and keeps an asynchronous IN
request pending to capture error replies or final verification while sending.
The user ran it with administrator access and reported three verified 4 MiB
runs at **945.955, 945.730 and 946.112 kB/s** (median 945.955). Each final
byte count was 4,194,304 and the FNV-1a hash was `efdc9dc5`.
Bypassing the CDC driver did not improve throughput in this synchronous
single-OUT-request test. The remaining limit has not been isolated to the
device, hubs or controller.

The next direct test keeps 4, then 16 asynchronous 32 KiB OUT requests queued
while retaining a pending IN request. It uses the same final integrity barrier
and cancels/drains outstanding requests before releasing the USB interfaces.
This tests whether waiting for each host transfer completion leaves the bus
idle; it does not add per-message application replies. Run the compiled local
diagnostic with `sudo /tmp/channel-cdc-optimization/bulk_probe --queued`.
The user reported the following six verified 4 MiB runs (all hashes
`efdc9dc5`):

| Queued OUT requests | Run 1 kB/s | Run 2 kB/s | Run 3 kB/s | Median kB/s |
|---:|---:|---:|---:|---:|
| 4 | 955.249 | 955.233 | 955.217 | 955.233 |
| 16 | 955.223 | 955.209 | 955.201 | 955.209 |

Increasing queue depth from 4 to 16 did not improve the result. Accounting for
the 5-byte header per 1024 useful bytes, the median corresponds to about
959.90 framed bytes/ms, or an average of 14.998 full 64-byte packets/ms.
The useful-data rate at exactly 960 framed bytes/ms would be
`960 * 1024 / 1029 = 955.335` bytes/ms, very close to these measurements.
This is an observed throughput plateau, not a USB trace proving exactly 15
packets in every frame or identifying which component imposes the limit.

The 998 useful bytes/ms target needs about 1002.87 framed bytes/ms with the
current header. Even eliminating the application header would not reach it
at the observed 960 framed bytes/ms plateau. Next diagnosis needs packet
timing/device backpressure instrumentation or a comparison on another USB
host/topology; a deeper host queue alone has not recovered the gap.

## Repeat the continuous benchmark

Requires Python 3 and pyserial; close other applications using the card first.
Use the board's current CDC path after reconnecting.

```sh
python3 channel_card/scripts/benchmark_usb.py \
  --port /dev/cu.usbmodem13301 \
  --output /tmp/channel-throughput.json
```

Local measurement logs are in `channel_card/build/throughput/`:
`before-continuous.json`, `after-continuous.json`, and `host-sizing.json`.
The diagnostic test leaves samples and voice state unchanged, but opens a new
USB protocol session. It must not run concurrently with the audio host.
