# Channel CDC migration: validation and hardware qualification

The migration changes one STM32H725 Channel Card and the `voice_bd` host. Eight
voices, signed-int8 source samples, 48 kHz DAC, script behavior, and RS485 controls
are retained. Effect Card firmware is unchanged. MAS uses the matching shared
`voicebd.cpp`/`voicebd.h` transport; its UI and synthesis logic are unchanged.

## Automated checks

From the repository root, with the STM32 toolchain and host dependencies installed:

```sh
cmake -S channel_card/tests -B /tmp/channel-usb-tests
cmake --build /tmp/channel-usb-tests
ctest --test-dir /tmp/channel-usb-tests --output-on-failure
cmake -S voice_bd -B /tmp/voicebd-tests
cmake --build /tmp/voicebd-tests
ctest --test-dir /tmp/voicebd-tests --output-on-failure
cmake --preset Debug -S channel_card
cmake --build channel_card/build/Debug
cmake --preset Release -S channel_card
cmake --build channel_card/build/Release
```

Firmware native tests exercise the production descriptors, endpoint-zero state
machine, CDC queues, parser, attack uploader and voice rings against a mocked
HAL PCD. They test multi-packet descriptors, address/configuration/line coding,
BODY interleaved with uploads, bad offsets, truncated frames, DTR recovery without endpoint toggle resets, retained BODY when the voice ring
is full, RX backpressure, terminating IN zero-length packets, and diagnostic integrity counters. HAL/register and physical
USB behavior still need a board. Protocol-v2 tests additionally cover inferred
note start from fragmented priming, stale notes, session and BODY-counter wrap,
HELLO counter baselines after reconnect, and rejection of v1 framing.

Host tests exercise every two-read frame split, concatenated frames, maximum
lengths, exact credit reconciliation, bounded five-ms forecasts, byte-window
limits for small blocks, old-session accounting, note promotion during a reserved
write, ring wrap/capacity, and sequence wrap. The actual firmware ring code is
linked to the host scheduler. Eight-voice simulations cover 30 seconds of
virtual time at 384 kB/s with 8 ms status gaps and 70 seconds at 960 kB/s with
5 ms status, crossing the implicit 16-bit BODY counter wrap. These are fast
software simulations, not timed hardware playback. A socket peer forces the
real host worker to use 17-byte writes/13-byte reads, interleaves upload replies
with BODY, and tests wrong reply types/targets, incompatible capabilities,
partial replies/disconnects, timeout without request reuse, and BODY errors
during uploads. HELLO tests replace stale host state with zero, nonzero and
wraparound firmware baselines.

Linux tests also check real worker scheduling and FTDI latency configuration.
Set `VOICEBD_EXPECT_REALTIME=0` when running without real-time permission, or
`VOICEBD_EXPECT_REALTIME=1` with an `rtprio` limit of at least 20, to require the
expected permission outcome. Serial and scheduling mocks live in the transport test executable; production
`voicebd.cpp` has no test-only branches. A separate worker verifies `SCHED_RR` priority 20
and confirms that denied scheduling prevents its work from starting.
Timer fixtures cover setting 1 ms, an already configured read-only timer,
missing adapter-specific attributes, and denied writes.

The standalone host, browser, transport tests and hardware-test executable
were compiled with GCC on Debian Bookworm ARM64 with warnings as errors.
Browser/transport tests passed as an unprivileged user, including scheduling
denial and permission via a container-scoped `rtprio=20` limit. This validates
the Linux code paths, not physical Rockchip USB/RS485 timing.

For memory/undefined-behavior checking, configure both native test builds with
`-fsanitize=address,undefined -fno-omit-frame-pointer` in C/C++ flags and
`-fsanitize=address,undefined` in executable linker flags.

## Hardware gate (not established by the automated checks)

1. Record the previous UAC baseline from `develop` at `92f879b`: identical sample,
   script, host, USB port/hub and eight-voice note workload. Capture note-on-to-DAC
   latency, USB delivery gaps, and RS485 `usb` counters. Save baseline firmware.
2. Flash the new Channel firmware using the existing DFU workflow. Verify
   `cafe:4032` enumerates as one CDC port and no audio device; check the unchanged
   `CHCARD-<UID>` identity. Update `voice_board_config_t::usb_port` for its OS path.
   Keep `rs485_port` pointing to the USB-to-RS485 adapter. Use the matched new host.
3. Exercise USB reset, unplug/replug, DTR reopen, suspend/resume, incompatible
   HELLO, interrupted script uploads and explicit protocol errors. Recovery must
   work without stale BODY reaching newly armed notes. Reopen the board after
   transport failure; `voicebd` reports I/O/protocol errors rather than retrying
   partially delivered BODY blindly.
4. Clear `c:usb 0` on RS485 before each playback test. Test silence, single notes,
   simultaneous eight-note starts, rapid replacement, pitch changes, release
   tails, hard stop, and live sample replacement. Verify the initial 998 samples
   are received before note-start dispatch, and no BODY is submitted after all
   voices finish. RS485 polling and USB housekeeping can remain visible at idle.
5. Stress eight nominal-rate voices (384 kB/s), then eight double-rate voices
   (768 kB/s). The existing engine folds rates above 2x down an octave; higher
   MIDI keys alone do not exceed this demand. The former 998 kB/s packet budget
   and native 960 kB/s scheduler simulation are not higher supported playback
   rates. Include a single fast voice and
   mixed pitches, host CPU/I/O load, and attack uploads during playback. Script
   upload while notes are active must still be rejected by existing VM rules.
6. For each supported load run at least 30 minutes. Require no increase in
   `hold`, `drop`, `full`, `bad` or `late`; investigate unexpected stale/future
   session counts. Measure p50/p95/p99 and maximum key-to-first-DAC and BODY
   delivery latency using the same clock/capture setup as the baseline. Transport
   throughput and latency must match or beat the supported UAC baseline before
   this migration is called hardware-qualified.

A USB trace can establish packet timing but cannot equate a host write return
with firmware consumption. Use a USB analyzer or correlate MCU trace/GPIO with
RS485 note acknowledgement and DAC output for end-to-end timing. Buffers absorb
measured jitter; they do not guarantee survival of unbounded host stalls or
source demand beyond the measured bus capacity.

The protocol supports up to 1024 sample bytes per block, not a guaranteed number
per millisecond. The current PHY remains 12 Mbit/s. Multi-card integration and a
480 Mbit/s external ULPI PHY are separate changes.

## Repeatable playback test

The opt-in test uses a quiet generated sine at 48 dB attenuation. It opens the
specified card, replaces its voice programs/attack sample, and stops notes on
exit. It is not part of automatic CTest runs. From the repository root:

```sh
cmake --build /tmp/voicebd-tests --target voicebd_hardware_test
/tmp/voicebd-tests/voicebd_hardware_test RS485_PORT USB_PORT voice_bd/series2.bec 1800 72 steady
/tmp/voicebd-tests/voicebd_hardware_test RS485_PORT USB_PORT voice_bd/series2.bec 60 72 exercise 20
```

Arguments after the BEC path are duration in seconds, MIDI key, mode and optional
reopen count. Long steady runs rearm the eight notes every 30 seconds to keep
nonzero source data throughout. Exercise mode repeatedly replaces/releases
notes, uploads attacks while playing, and checks that active script replacement
is rejected without breaking USB. Both modes require zero hold/drop/full/bad/
late/future counters, no RS485 error increase, unchanged USB bytes over two
seconds of idle polling, and working RS485 stop commands after USB closes.

On macOS both transport workers request interactive QoS and a preemptible
real-time policy. Plain thread scheduling produced measured 19–26 ms worker
gaps and underruns despite sufficient average USB throughput. The Linux build
is tested separately; Linux hardware scheduling still needs qualification.

USB queue critical sections prevent the mixer from preempting main-loop code
while USB alone is masked. That priority inversion reduced measured payload
throughput under mixer load. RS485 TX feeds its FIFO from the existing UART
interrupt, while the waiting main loop continues dispatching USB blocks.
RS485 `c:rs485` reports receive drops, transmit timeouts and truncated replies.

## USB enumeration diagnostics

RS485 `c:usbdev` reports reset/setup/stall counts, control stage, configuration,
DTR, suspend/fault state and the last eight setup bytes. The trailing register
values are GINTSTS, GINTMSK, GAHBCFG, DCFG, DCTL, GOTGCTL and GCCFG, in that order.
This permits USB startup diagnosis while CDC itself is unavailable.

On this DWC2 controller, set the device address before submitting the control
status ZLP, consistent with [ST's SET_ADDRESS implementation](https://github.com/STMicroelectronics/stm32-mw-usb-device/blob/master/Core/Src/usbd_ctlreq.c).
Deferring the register write until the IN-completion callback failed enumeration
on the connected Mac: repeated resets stopped after SET_ADDRESS. Native HAL
mocks alone did not catch that controller behavior.

DTR transitions discard application queues but must preserve endpoint DATA PID
state. Closing/reopening endpoints on a CDC line-state change resets only the
device toggle and can lose the next host packet. Recovery now keeps OUT armed
and aborts/flushes queued IN data without reopening endpoints. Protocol faults
leave OUT NAKed rather than using a halt that would reset toggle state on clear.


## Protocol-v2 validation — 17 September 2026

The five-byte header passes native firmware and host tests, ASan/UBSan tests,
and Debian Bookworm ARM64 tests with real-time permission both granted and
denied. The STM32 Release build and matching ARM64 MAS are rebuilt together.
The packet contract and hex examples are in [the packet guide](usb_packet_guide.md).
These checks do not establish hardware playback or latency for v2. Start with
a short 30-second playback/exercise check on the Rockchip; the longer release
qualification below remains separate.

## Recorded v1 hardware results — 17 September 2026

These measurements used the previous eight-byte header. They must not be
reported as hardware qualification of protocol v2.

Board: `CHCARD-001B00253034510A31373233`, STM32 DFU serial `315232583034`.
Final Release binary: 167372 bytes, SHA-256
`90676579620837f39a0ef572c77070cf8d92aa6a20a3764eaf0d54d584c4470e`.
The complete flash read-back matched. One CDC data port enumerated; no Channel
USB Audio interface was present. Status/credit stayed on RS485 throughout.

* Final firmware, Series 2 program, eight voices at 2x: the planned 30-minute
  run was shortened at the user's request after approximately 232 seconds.
  The last periodic check was at 230 seconds. Final `drop`, `hold`, `full`,
  `bad`, `late`, `future`, RS485 `rxdrop`, `txfail` and `trunc` were all zero.
  Minimum ring fill was 638 samples. Final receive count was 181108072 bytes;
  it remained unchanged after stopping playback.
* Before the final volatile-field safety change, the same transport passed
  30 seconds at nominal rate, 60 seconds at 2x, and two 30-second folded-pitch
  runs with no error-counter increments. A 30-second Series 2 exercise run
  passed rapid replacements, live attacks, active-script rejection, 20 reconnects,
  two seconds of open-port idle, and RS485 stop after closing USB. Its minimum
  ring fill, including reconnect starts, was 517 samples.
* Verified USB probe throughput with the mixer active and 5 ms RS485 polling:
  900.0 kB/s with eight-block batches and 925.4 kB/s with 32-block batches.
  For 64 single-block/barrier observations, round-trip p50 was 1.659 ms,
  p95 1.888 ms and maximum 1.965 ms. This benchmark ran in the caller's normal
  scheduling class; it is not a note-on or one-way latency measurement.
* macOS native, address/undefined-behavior sanitizer, Linux ARM64 native and
  STM32 Debug/Release builds/tests passed. The Linux hardware harness compiles;
  no Linux-connected Channel Card was tested.

Session log: `channel_card/build/cdc/qualification/steady-1800s.log` (the
filename reflects the planned duration, not completion of 1800 seconds).
Older-session blocks counted as stale during replacement/stop are discarded
by design; they are not ring overflow or protocol failures.

These results establish working transport under the tested loads. The full
30-minute release soak, multi-card load, physical unplug/suspend tests and
previous-UAC versus CDC note-to-DAC timing comparison remain outstanding.
No oscilloscope/audio-capture measurement establishes a 1 ms end-to-end bound.

## Recovery and rollback

USB failures do not reset the MCU or stop DAC clocking. Stop notes over RS485,
close the host, reconnect/reopen the CDC port and reload/rearm as needed. Upload
aborts preserve existing semantics: script replacement is validated before
publication, while attack data can have been partially overwritten.

Firmware and host must be rolled back together to `92f879b` to restore UAC.
The new branch is `feature/channel-usb-cdc-stream`; no remote push is implied.
