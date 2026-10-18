# Current underrun repair

> Current status (19 October 2026): shared Channel Card refactoring and full MAS
> compiled-mode playback have software verification. Work is **not complete**:
> the shared disconnect stop is approved, applied, and verified;
> the separate MAS interpreter token race remains unfixed (MAS changes are
> restricted to test copies except the previously approved timer correction).
> Later dated evidence below supersedes earlier completion/blocked statements.

The chronological investigation below includes superseded hypotheses and experiments.
The current production changes are:

- Refill from confirmed free-space credit instead of stopping at an arbitrary
  10 ms lookahead. No buffer capacities increased and no transaction timeouts added.
- Derive streaming-active flags from the same stream state as session IDs.
  A captured reply after clear contained active=1 with current/target=255;
  the host rejected it and waited indefinitely. A deterministic retirement
  interleaving test fails before this fix and passes afterward.
- Require the existing 998-sample preparation before both idle and replacement
  note-on handlers. Previously an active voice could dispatch a script which
  started an unprepared replacement immediately. The old note continues while
  preparation completes. Host prefetch now preserves that same requirement.

The intermediate refill-timer experiment was replaced by direct credit-driven
refill; it added complexity and did not cover all observed starvation paths.
The buffer size remains 4080 samples per voice. Latency still has a finite
physical budget: software tests cannot guarantee delivery under arbitrary host
or transport stalls. Hardware is unavailable.

- Bound underrun-tail processing to the samples available at entry. Previously
  concurrent BODY arrivals could prolong the same audio callback indefinitely.
  Later arrivals now remain queued for normal playback; buffer sizes and the
  existing output fallback are unchanged. The production regression fails before
  the repair and passes afterward: one initial sample produces exactly one peek
  with injection budgets of 1, 100 and 10,000 concurrent arrivals.

Current validation:

- After the tail repair, the full channel suite and STM32 build pass. Envelope,
  oscillator and fault PCM hashes are unchanged. All four 2-second production
  host/simulator phases pass under ThreadSanitizer with zero holds/drops and no
  reported data races. Logs: `/tmp/channel-tail-suite.log`,
  `/tmp/channel-tail-board.log`, `/tmp/channel-tail-tsan.log`.
- After removing per-packet simulator debug logging and the unused host origin
  field, the host stream test and all four 2-second integration phases pass.
- Immediate replacement (zero fade) and 50 ms replacement-fade variants each
  pass all four 2-second phases with zero holds/drops. These extend the normal
  3 ms envelope coverage; they are software evidence, not new hardware results.
  `streaming.py --program-source` selects the envelope explicitly. Logs are
  `/tmp/channel-fade-immediate.log` and `/tmp/channel-fade50.log`; variant sources
  are in `/tmp/channel-fade-variants/`.

- `/tmp/channel-underrun-final`: four 10-second production-host/simulator phases
  (one C4 voice, eight C4 voices, eight C5 voices, 200 retriggers), all zero
  holds and drops; minimum observed fill stayed positive in every phase.
- `/tmp/channel-underrun-tsan`: all four 2-second phases pass with zero holds
  and drops; no ThreadSanitizer warnings in the simulator log.
- Full `scripts/tests/channel_card/run.py` suite passes, including envelope,
  audio partition, transport, serial backend, protocol and boundary checks.
  Replacement preparation intentionally changes onset timing; the partition
  tests still produce identical output across callback sizes.
- Five host CTests pass. Deterministic refill covers eight source rates for
  100 virtual seconds with 10/20 ms delayed status replies and exact credit.
- Current STM32 board build and `git diff --check` pass. No hardware flashed.
- Clean standalone host build (without the firmware tree) builds CLI, GUI and
  all four standalone CTests successfully. It caught an unused polling constant
  under `-Werror`; that dead constant was removed. All 30 channel build/release
  Python tests also pass. Logs: `/tmp/channel-standalone-recheck.log` and
  `/tmp/channel-underrun-packaging.log`.

A separate artificial 15 ms status-delay experiment before the final preparation
change still starved at eight C5 voices (actual proxy delays exceeded the requested
15 ms). That fault-injection result is retained; the 40-second normal workload
and deterministic cadence tests are not guarantees for arbitrary transport/OS
stalls. Physical timing qualification remains outstanding.
The approved session ownership repair is now implemented: audio alone changes
current/pending roles, using a single publication instead of retry loops.
Transport reserves storage and transfers its index in the existing note-command
word. A second command for that voice stays in the existing console line and RX
buffer until publication; different voices can share the same audio boundary.
No host acknowledgement, extra queue, sample buffer or blocking wait was added.
Historical pending-approval and prototype notes below describe earlier checkpoints.

---

# Channel streaming investigation — 2026-10-18

## Scope and evidence

The reported failure occurs with MAS sample playback on both targets. No Channel
Card hardware was available. Changes are not a hardware-qualified underrun fix.
The user authorized shared transport buffering and explicitly requested that
existing transaction timeouts be disabled unless configured.

The pre-change source is `ccf3e3f`. `11e6581` remains an unverified historical
baseline. The local simulator executable initially did not link PortAudio,
although current source requires it; it could not establish current behavior.
The simulator build target subsequently rebuilt that ignored executable. The
firmware build used isolated `/tmp/channel-transport-board` outputs and was not
flashed. PortAudio development files were installed to run native audio tests.

## Confirmed findings

- STM32 and PTY adapters maintained separate byte-buffer algorithms. The PTY
  implementation compacted bytes after every read. They now use one queue
  implementation and shared USB lifecycle state; device staging stays in adapters.
- The host imposed a 5 ms RS485 reply deadline, a 3 s USB reply deadline and a 1 s
  USB write deadline. Firmware imposed 1 s partial-frame and 5 s upload deadlines,
  and derived RS485 TX deadlines. All are now opt-in. A focused test accepts a
  delayed 20 ms status reply by default and rejects it with an explicit 5 ms limit.
- ThreadSanitizer reported 41 data-race warnings in the threaded simulator.
  One concrete conflict is `commit_voice_script_upload` calling
  `stop_voice_immediately` on the main thread while an audio-block clear calls it
  on the audio thread. Races also involve active-note queries, stream release,
  oscillator state and playback-duration queries. Volatile fields and the host's
  no-op interrupt mask do not provide inter-thread synchronization.

## Verification so far

- The shared queue tests, serial adapter tests, protocol tests, audio partition
  tests, native envelopes, boundary tests and DAC trace passed.
- The STM32 `board` target compiled and linked; physical operation is untested.
- The real host streaming worker passed a 2 s single-voice and 2 s eight-voice
  software run with zero holds, BODY drops and protocol errors. The same workload
  also passed against the pre-change host and simulator source snapshot.
- A longer 10 s single-voice software run reported 62 holds with zero drops and
  protocol errors. This confirms intermittent sample starvation in the software
  setup, but does not localize its cause. The software audio clock can catch up
  after scheduling delays, unlike a physical DAC.
- An isolated 10 s single-voice plus 10 s eight-voice run with bounded callback
  tracing passed with zero holds/drops/errors. Maximum callback execution was
  440 microseconds; maximum callback-start gap was 9.17 ms. The earlier failure
  remains intermittent and is not explained by this passing repeat.
- Native PortAudio oscillator and envelope tests passed with zero voice holds
  and BODY drops. One measured RS485 round trip was 7.96 ms, exceeding the former
  implicit deadline. This does not establish that a timeout caused the reported
  underruns.
- The sanitizer run reported 3064 holds at eight voices. Its instrumentation
  overhead prevents treating that timing result as a production performance
  measurement. The race reports themselves are actionable correctness evidence.

## Approved concurrency repair

The user approved audio-owned mutation and required nonblocking coordination.
Packed atomic note commands replace separately published command/key/velocity
fields. The renderer publishes voice and VM diagnostics through fixed-size
three-slot snapshots; the protocol thread owns reading. Stream consumption and
audio counters use atomic publication. The shared VM changes ownership only when
there are no active or queued notes. A complete upload-begin frame waits in the
parser for an idle renderer to relinquish ownership, without spinning or blocking
the callback. Rendering continues its idle clock while upload owns the VM.
Upload commit requests renderer cleanup rather than mutating voices concurrently.
The existing restriction against uploading scripts during active playback stays.

A regression in the first snapshot implementation was reproduced and corrected:
status showed `active=0 pending=0 current=1` during pending-to-current promotion.
The host retired the note and sent no further BODY packets, producing over 94,000
holds in two seconds. Reading both session roles from one ring publication and
marking a current session active eliminates this gap. A deterministic protocol
test exercises promotion before the voice snapshot is published.

Final checks include render-during-upload and busy-handoff tests, a million
concurrent snapshot publications, audio partition equivalence, protocol and
transport failure paths, and the host envelope/stream/RS485 tests. The snapshot
stress test passes ThreadSanitizer. A 10 s single-voice plus 10 s eight-voice
production-host run passes ThreadSanitizer with zero warnings, holds, drops or
protocol errors (`/tmp/channel-ownership-final-tsan`). The repaired STM32 firmware
compiles and links. Four 10 s phases (one voice, eight voices, eight voices at
MIDI 72/double source rate, and replacement every 50 ms) pass with zero holds and
drops in `/tmp/channel-ownership-patterns`. Two-second versions of all four also
pass with no sanitizer warnings in `/tmp/channel-ownership-patterns-tsan`.
Replacement phases correctly count stale packets from retired sessions (74 and 4
respectively); these are not reported as BODY overflow or malformed frames.
A later native PortAudio attempt could not run because the
system had no default output device; the earlier native results above predate the
ownership repair.

## Limits

The original intermittent 62-hold run remains unexplained; the confirmed races
are correctness defects, but these results do not prove they caused every reported
underrun. Retain failing traces rather than increasing buffers or hiding errors.
Software scheduling and ThreadSanitizer change timing. Hardware-specific USB/DMA
and RS485 behavior remain unverified until a board is available. Concurrent attack
bank replacement retains the existing documented in-place update behavior; this
repair does not redesign sample-bank ownership.

## Simplicity and STM32 build audit

Removed the duplicate `*_channel_script_upload*` API layer from `voice.cpp` and
`voice.h`. Only the ownership-checked `*_voice_script_upload*` entry points remain;
backend calls and validation now sit together. The unchanged complete software
suite passes after this refactor. The production-host four-pattern streaming
check also passes ThreadSanitizer with zero holds/drops in
`/tmp/channel-simplified-tsan`.

The STM32 linker map showed nine compiled driver translation units with no
allocated output sections: DMA extensions, EXTI, flash and flash extensions,
HSEM, I2S extensions, MDMA, base PWR and SAI extensions. Removed these source files
and their explicit Makefile entries (8,369 source lines). A clean before/after
build has identical binary SHA256:
`2f7993a3a48798ca716a40a39635148fef811bfad1b6a4f5cf1275d0e6aea99e`.
Evidence: `/tmp/channel-trim-before` and `/tmp/channel-trim-after` (maps, binaries,
objects), with corresponding `.log` files. All remaining core/driver objects have
allocated output sections. This reduces source/build bulk, not flash usage:
the linker was already discarding the unused functions. HAL headers remain where
needed for types, register macros and dependencies of retained drivers.

### Pending real-time bound review

A diagnostic-only interleaving test in `/tmp/channel-audio-audit` invokes the
actual underrun handler with one BODY arrival before each tail peek. A single
call performs 2, 101 and 10,001 peeks for 1, 100 and 10,000 injected arrivals.
Thus the drain-until-empty loop has no producer-independent work bound in the
threaded simulator. This is not proof that it caused the earlier observed holds.
The proposed repair captures the available count once and drains only those
samples, leaving later arrivals for normal rendering. Approval was requested
under AGENTS.md before changing this behavior. Production behavior remains
unchanged pending that decision. Stream-state compare/exchange loops also need a
bounded-work audit; lock-free by itself does not prove a hard callback deadline.

The subsequent behavior-preserving cleanup removes the duplicate native callback
struct/copy used only during initialization and the uncalled immediate-stop API.
The renderer-owned stop request remains. Full software tests, the three host
integration/unit tests and the STM32 build pass after removal.

An isolated proposed fix was tested in `/tmp/channel-audio-audit/voice_bounded.cpp`
without changing the production underrun handler. With one initial tail sample
and arrivals injected before each peek, the proposed handler performs one peek
and leaves one newly arrived sample queued, for injection budgets of 1, 100 and
10,000. Results are in `bounded-result.txt`. The user requested clarification of
this proposal; approval is still pending. This experiment proves the loop bound
for that interleaving, not the cause of ordinary playback starvation.

The remaining channel-script forwarding functions and declarations were removed;
voice commands/queries now have one public interface. Backend boundary/reset and
profiling calls remain at the same points in the renderer. The full software
suite, three host tests and STM32 build (`/tmp/channel-script-api`) pass unchanged.

The stream-state bound audit has a deterministic reproduction in
`/tmp/channel-ring-audit/promotion.cpp`: before each audio promotion CAS, a
producer publishes another pending session using the actual production API.
For 1, 100 and 10,000 replacements, one promotion takes 2, 101 and 10,001 CAS
attempts respectively. This demonstrates no producer-independent attempt bound
in the threaded simulator. It does not establish observed callback latency or
prove the same scheduling can occur on STM32, where the main-loop producer is
preempted by audio interrupts. Replacing this loop requires preserving atomic
session visibility and queue ownership; simply limiting retries would change
note behavior and is not an approved fix.

## Historical completion audit (before the approved repairs)

| Requirement | Evidence | Status |
| --- | --- | --- |
| Shared simulator/hardware byte transport | Both adapters use `transport_buffer.h`; queue/backend/protocol tests pass | Implemented |
| Explicit transaction expiration only | Zero-default host/firmware policy; delayed replies, cancellation and opt-in expiry tests | Implemented for streaming transports |
| Smaller STM32 build/source set | Nine unused units removed; clean binary equivalence; retained objects allocate sections | Verified for configured STM32 build |
| Simpler script boundary | Duplicate structs, unused teardown and forwarding API removed; unchanged tests and board build pass | Implemented |
| Safe concurrent voice/upload/status access | Ownership tests and recorded ThreadSanitizer playback runs | Verified for tested workloads |
| Fixed callback work under concurrent traffic | Tail-drain and promotion-CAS interleavings exceed fixed initial-work bounds | Not achieved; behavior changes pending discussion |
| Reported starvation fully explained | Passing sustained software patterns; earlier intermittent 62-hold run remains unexplained | Not achieved |
| Physical USB/DMA/RS485 timing | No board available; native audio currently unavailable | Unverified |

At that checkpoint the goal was blocked pending agreement on the newly discovered behavior changes.
The same approval dependency remained unresolved across three consecutive goal
turns; the independent cleanup and verification are finished. Passing tests do
not close the two known work-bound issues or establish the root cause of every
originally reported underrun.

## Software-clock catch-up investigation

The root-cause investigation resumed independently of the pending underrun-handler
proposal. The test-only fake PortAudio backend uses an absolute `sleep_until`
clock. When delayed it issues overdue callbacks without waiting, whereas the
physical DAC cannot consume past audio in an instantaneous burst.

Added an explicit, zero-default `--audio-stall-ms` diagnostic to `streaming.py`.
It affects only `tests/fake_portaudio.cpp`, never production audio or firmware.
In `/tmp/channel-clock-stall`, a 100 ms injected callback-thread pause produced a
107.696 ms measured callback-start gap. The following 38 callbacks rendered 4,864
frames (101.33 ms of 48 kHz audio) in 0.242 ms. The first resumed callback saw
3,142 queued samples; that fell to 70 before the first miss and then to zero.
The single-voice phase reported 2,107 holds, zero BODY drops, and zero malformed
frames. The failure is deliberately retained; no errors were suppressed and no
production buffering, scheduling or recovery policy was changed.

This confirms that an unrestricted software link can underrun under the fake
clock's catch-up burst. It does not prove the original 62-hold run had the same
cause: that earlier run has no callback trace. The normal production simulator
links actual PortAudio, so this injected-clock result must not be attributed to
that simulator or hardware. Subsequent streaming conclusions must distinguish
sample starvation during normal callback cadence from fake-clock catch-up after
a scheduling gap.

The no-injection control (`/tmp/channel-clock-control`) passes all four 2 s
patterns with zero holds/drops; maximum callback duration is 1.012 ms and maximum
start gap is 4.001 ms. Repeating all four 10 s patterns concurrently with the full
software suite also passes with zero holds/drops (`/tmp/channel-concurrent-trace`;
16008 callbacks, maximum start gap 10.538 ms, maximum render 1.412 ms).
The concurrently executed software suite passes (`/tmp/channel-concurrent-suite.log`).
Thus the original 62-hold condition remains unreproduced in the current code;
the fault-injection result explains a concrete test failure mode, not the original
hardware/simulator report. No production underrun policy changed in this work.

## Host refill-state cleanup

The refill-rule audit found `refill_balance`, `refill_at` and `forecast_until`
were never written after default initialization. The only forecast reader was
`refill_wake_ms`, whose positive-wakeup branch was unreachable on the supported
running host clock. Removed these three fields and that polling helper. The USB
worker still waits for I/O, MIDI/status wakeups and explicitly configured request
or write deadlines. No refill coverage, packet size, credit or scheduling interval
was changed.

On the current host, measured `sizeof(voice_data)` falls from 80 to 56 bytes and
`sizeof(stream_state)` from 182,184 to 133,032 bytes: 49,152 bytes (48 KiB) of dead
bookkeeping removed. The old/current size probes are retained as
`/tmp/channel-refill-size-before` and `/tmp/channel-refill-size-after`.
All three host tests pass. The four 2 s production-host streaming patterns also
pass with zero holds and drops in `/tmp/channel-refill-cleanup`. These results
verify the simplification; they do not identify the original starvation cause.

## Deterministic refill accounting

`voice_bd/refill_test.cpp` connects the production host refill logic to the real
firmware stream rings without OS scheduling or a USB timing model. Eight voices
consume independently at 15, 30, 60, 120, 240, 360, 479 and 480 source samples per
5 ms. Both immediate delivery and a fixed 5 ms delivery delay pass 100 virtual
seconds with no starvation or dropped blocks, each delivering 278,716 BODY
frames and crossing status/BODY acknowledgement sequence wraparound.

The envelope, refill, stream and RS485 host CTests pass. An unfiltered CTest
invocation also selected the GUI test, whose executable was not built in this
build directory; that test did not run. This deterministic result checks credit
accounting under the stated cadence; it does not model host stalls, physical USB
service latency, callback scheduling or prove the original underrun cause.

## Status-wakeup latency diagnostic

An isolated variant of the refill test in `/tmp/channel-refill-latency/` varies
BODY delivery and status-response latency independently, after five virtual
seconds of steady playback. The final model permits one status transaction at
a time and runs host packet preparation on status delivery, matching the idle
USB worker's wake dependency. Earlier exploratory variants incorrectly prepared
packets every tick despite no wake; their results are superseded.

With 5 ms consumption steps and the same eight rates as the refill test:

- Immediate USB delivery with 0 or 5 ms status latency passes 100 virtual seconds.
- Immediate USB delivery with 10 ms status latency starves at tick 1002: voice 1
  has 20 samples while the next step requires 30.
- A 5 ms USB delay with 0 or 5 ms status latency passes; a 10 ms USB delay
  with 5 ms status latency starves.

`usb_link::run` waits on I/O/control events once `make_block` has prepared its
lookahead; `query_status` applies the reply and wakes USB. This demonstrates a
software scheduling dependency that can starve playback despite unlimited USB
throughput. It does not establish the original failure cause or an exact
hardware latency threshold: the model advances in 5 ms steps, consumes before
processing status at a coincident tick, and omits OS/serial execution timing.
No production scheduling, buffers or recovery behavior was changed. Next useful
evidence is a traced integration reproduction of delayed status replies with
the actual host workers, distinguishing missing wakeups from queue saturation.

## Actual-worker status latency experiment

A fresh current-source build/run in `/tmp/channel-status-base` unexpectedly
reproduced an **unforced** failure before adding a proxy: single-voice and
eight-voice C4 phases passed, then eight C5 voices reported 65,768 holds, zero
drops and zero malformed frames. The first miss was callback 2481. All eight
rings fell together from 1094 samples at callback 2477 to 198 at callback 2481,
then zero. Misses spanned 65 callbacks over 171.279 ms. The largest callback
start gap anywhere in the run was 4.005 ms, so the previously demonstrated
large fake-clock catch-up burst does not explain this run. The current code's
intermittent starvation is therefore reproduced; its upstream cause remains
unlocalized. No status timestamps were collected on that direct run.

An external test-only PTY bridge at `/tmp/channel-status-diagnostic/proxy.py`
then intercepted RS485 replies while leaving USB connected directly. It forwards
complete 61-byte status frames in order, supports partial writes, records receipt
and release timestamps, and explicitly delays status after 100 active replies.
No production source or binary was modified. The zero-delay bridge control
(`/tmp/channel-status-control`) passed the first three phases but recorded 258
holds during retriggering. Python monotonic timestamps in that initial control
use a different macOS clock origin from simulator CLOCK_MONOTONIC; later runs
use clock_gettime_ns(CLOCK_MONOTONIC) for direct comparison.

With an explicit 15 ms requested status delay (`/tmp/channel-status-delay15`),
one C4 voice reported 5748 holds, zero drops and zero malformed frames. At the
first miss, fill fell from 454 to 326 to 198 to 70 over successive callbacks,
then zero. The relevant status was received 6.534 ms before the first miss but
released 13.860 ms afterward (20.394 ms actual delay, including proxy scheduling).
Maximum callback-start gap was 4.005 ms. This demonstrates actual host-worker
starvation while awaiting status with no USB link restriction and no large
audio-clock catch-up. It does not prove that the unforced run had the same
cause. The next diagnostic should timestamp host status requests/replies and
BODY preparation on the direct connection to locate its prolonged feed gap.

## Direct host decision tracing

`/tmp/channel-host-trace/` contains a separate host source copy and diagnostic
binary. A bounded 300,000-row trace records CLOCK_MONOTONIC timestamps for
status request/reply, decoded status, USB preparation, poll entry/exit, writes,
and no-eligible-packet decisions. No production source was edited.

- Run 1 reproduced 2792 reported holds in eight C5 voices, but audio tracing
  identifies a 31.912 ms callback-start gap followed by back-to-back renders.
  Host status requests/replies continued approximately every 5 ms through it.
  This failure is the known fake-clock catch-up mechanism, not evidence that
  the earlier normal-cadence failure had that cause.
- Run 2 passed all four 4-second patterns with zero reported holds/drops.
- Run 3 reached the explicit 60-second test watchdog. Audio continued for
  60.005 seconds, with no holds recorded and empty final rings. Host tracing
  was then only dumped at normal exit, so termination lost the host trace.
  This is an unresolved liveness failure, not a playback pass.
- Diagnostic tracing was changed to bounded shared mapped storage so forced
  termination retains rows, and host stdout is saved during execution. Runs 4
  and 5 each passed all four 4-second patterns with zero reported holds/drops.
  Their trace counts (277,597 and 279,584) stayed below the 300,000-row limit.

These results do not close the unforced normal-cadence starvation or the
watchdog failure. Both require a failing direct-host trace to localize.

## RS485 write cancellation defect

A focused actual-host test completed 2000 sequential `all_notes_off`/`clear`
transactions and close successfully (`/tmp/channel-clear-trace1`). This does
not reproduce or explain the earlier 60-second liveness failure.

A separate PTY saturation test confirms an independent defect in current
`rs485_link`/`serial_port`: after opening the link, filling the PTY output queue
(1024 bytes on this run), and issuing `send_event("clear")`, the write remains
pending. Calling `rs485_link::cancel()` still leaves it pending. Closing the
master releases it with `PTY write failed: Input/output error`. The test used
explicit 100 ms observation windows and a 2-second teardown watchdog; these
are test checks, not production timeout changes. Both the instrumented host
copy and unmodified repository implementation reproduce it. The latter test
is `/tmp/channel-cancel-original.cpp` and `/tmp/channel-cancel-original`.

Code confirms why: cancellation changes the RS485 atomic flag used by reply
reads, but `serial_port::write` waits for output readiness without observing a
cancellation event. Native `sp_blocking_write` likewise receives zero (unlimited)
by default. Cancellation cannot release that wait. This provides a concrete
shutdown-stall mechanism under output backpressure, but the original stalled
run did not retain enough evidence to attribute its cause to this defect.

Proposed repair: use nonblocking writes with an event wait that also wakes on
explicit cancellation, preserving configured write expiry and partial-write
accounting. No fixed polling delay, implicit timeout or audio-thread wait is
needed. User agreement requested under AGENTS.md before changing production
behavior. The goal remains incomplete.

The expanded direct-host trace in `/tmp/channel-host-trace-run6` records
command writes and reply reads as well as refill decisions. All four 2-second
patterns passed with zero reported holds/drops; 3446 measured status replies
had a maximum request-to-reply interval of 0.860 ms. This passing run
does not supply the missing failing trace. The RS485 cancellation repair,
audio underrun-loop bound and session-promotion bound remain unapplied pending
agreement; no new production behavior was changed.

## Approved RS485 cancellation repair

After the user's explicit approval, `serial_port::write` now uses nonblocking
writes for PTYs and native libserialport devices, preserving partial progress.
When output space is unavailable, poll waits on the port and the RS485
cancellation pipe. Cancellation remains latched until reopen. Configured write
expiry still applies; zero remains unlimited. No audio code was changed.

Shutdown's final unacknowledged clear is a single nonblocking attempt after
worker cancellation, so it can still be sent on a writable port without entering
another indefinite wait. Delivery of that final clear is not guaranteed under
backpressure; explicit all-notes-off remains the acknowledged operation.

The new actual-PTY `voicebd_serial_write` regression test verifies saturated
output cancellation without disconnect, latched cancellation, reopen, explicit
write expiry, exact byte preservation across partial writes and nonblocking
shutdown clear under saturation. Native serial stubs now return three-byte
partial writes to verify the same loop. Envelope, refill, stream, RS485 events
and serial-write CTests all pass. `git diff --check` passes.

The current-source streaming run in `/tmp/channel-cancel-streaming` passed
single C4, eight C4 and eight C5 voices with zero holds/drops, but retriggering
reported 20 holds and zero drops/malformed frames. Prior runs also failed in
retriggering; this does not establish either a new regression or its absence.
The cancellation defect is fixed for the tested paths; the original streaming
issue remains unresolved. Physical serial hardware was unavailable.


## Remaining session ownership repair — proposed, not implemented

Rebuilt the diagnostic against current `stream.cpp`: 1, 100 and 10,000 producer
replacements cause 2, 101 and 10,001 attempts in one audio promotion. This is a
reproduced lack of a fixed work bound, not a measured recurrence of playback
underruns. `queue_voice_note_with_session()` prepares/publishes transport-side
pending state; audio-side activation, discard, retirement and reset all compete
for that same packed state. A failed activation becomes a script error, so a
retry cap is not a valid repair.

Proposed ownership contract:

- Keep block allocation, BODY writes and retired-block reclamation on the
  transport thread. Do not move storage reclamation into the audio callback.
- Transport prepares an unpublished queue and passes its index plus the complete
  note command to audio. Hold that queue until audio acknowledges the handoff.
- Audio alone publishes current/pending roles, applies replacements, and retires
  queues. Consume at most one prepared request per boundary. Existing sample
  queues remain shared SPSC storage; no second copy of BODY buffers is needed.
- Keep normal `n...` note events silent. The host's `send_event()` deliberately
  does not read a reply; firmware suppresses replies for those commands. The host
  authorizes new-session BODY only after a matching `vq` snapshot.
- Do not overwrite an outstanding preparation. Retain later RS485 input until
  audio has consumed the handoff, while continuing USB service and playback.
  Normal host note events can be pipelined: retained-input and status-query order
  are mandatory test cases, not an exceptional raw-client case.
- Keep 998-sample readiness before the script receives note-on, and maintain
  coherent status snapshots throughout the handoff. Cover stop, clear, upload,
  session supersession and disconnect before removing the old role CAS loops.

Observable consequence: a new pending session appears in status after the audio
boundary instead of immediately on transport preparation. Normal note events
remain silent. Audio callback work must not depend on how often a producer
attempts to replace the note. The previous acknowledgement-timing proposal was
incorrect and is superseded by this host-path inspection.

Production ownership changes remain proposed under AGENTS.md; no acknowledgement
change is needed for normal host events. Current production behavior is unchanged.
The protocol/README were corrected separately to describe the already implemented
998-sample preparation for both idle and replacement notes.


### Host compatibility check for deferred publication

`voice_bd/stream_test.cpp::check_delayed_session_publication` now exercises the
actual host scheduler through 100 fresh pre-publication status snapshots, with
100 seconds of virtual elapsed time. The note remains queued, no BODY is emitted,
and its source cursor/priming count remain unchanged. Publishing the matching
pending session then permits BODY immediately. The test passes together with the
existing outgoing/replacement credit tests and 5,376 status encoding combinations.
This proves the host's deferred-publication behavior, not the proposed firmware
handoff or its callback bound. The latter still needs implementation and testing.


## Shared build boundary after the simplicity review

Both targets already link the same protocol, voice, stream and mono renderer
implementations, use `TransportByteQueue` for their byte queues, and supply their
own peripheral/callback adapters. Their duplicate application source lists are
now consolidated in `app/sources.mk`; STM32 and simulator Makefiles add their own
adapters. Ordered source lists match the pre-change lists exactly. Both targets
build successfully. The release exporter now includes and requires the manifest,
so exported firmware and simulator builds retain the same boundary.

This is build-only consolidation. It introduces no runtime queue, buffering,
handshake or scheduling change. The session-state retry issue remains open.


## Isolated single-owner ring prototype

To keep the repair within the existing note flow, a diagnostic copy at
`/tmp/channel-session-owner-prototype/stream.cpp` splits transport-side queue
reservation from audio-side publication. Audio is the sole role-state writer;
activation, discard, end and reset therefore use one state publication instead
of compare/exchange loops. Storage allocation, BODY writes and reclamation stay
on transport. No additional sample queue or capacity is introduced.

The diagnostic harness models an ownership marker in the existing note-command
handoff. Transport retains later input while that marker is occupied; audio
releases the marker only after publishing the session and retiring its old
pending queue. Clearing it earlier would allow premature producer reuse.
Normal silent note events and status-driven BODY remain the intended contract.

`check.cpp` passes offers of 1, 100 and 10,000 subsequent requests while a handoff
is occupied, with no producer role-state writes. It also checks promotion while
a replacement is staged, intact playing samples, pending discard, current end,
and 100,000 lifecycle transitions returning all existing blocks to the pool.
This is a deterministic test of a modified ring copy, not a threaded test or a
production command-path fix. The live `app/stream.cpp` is unchanged. Integration
must reuse the existing command publication and cover retained RS485 input,
clear/off and upload ownership; adding an independent command queue is not the
proposal. The original compare/exchange reproduction remains open in production.


## Approved production session ownership repair

The user explicitly approved applying the simpler repair. Production ring role
changes now have one audio owner (plus cold initialization and inactive upload
commits with exclusive runtime ownership). Removed the role CAS loops and their
unused version bits. Transport retains storage allocation, BODY writes and
reclamation. The existing packed note command carries the reserved queue index
in unused low-byte bits; audio clears that marker only after publication and
retirement. Existing single-attempt command CAS operations retain supersession
semantics without retry loops.

The console retains a same-voice command in its existing line buffer until the
handoff is available. It returns to the main loop in the meantime. Distinct voices
can be posted together; normal note events remain silent and `vq` continues to
report coherent published sessions. BODY is authorized by matching status as
before. Direct note APIs return their existing busy result if a caller attempts
to overwrite a reservation; the serial parser retains input instead of dropping it.

Verification covers the actual boundary at sample 48, BODY readiness at the next
boundary, zero-length callbacks, eight voices per boundary, same-voice pipelining,
status before/after publication, upload exclusion and clear. A deterministic
interleaving posts another reservation after the first publication pass and then
requests clear. Clear consumes this reservation before erasing the command.
The isolated build without that cleanup traps when all three queue roles are
reused; production with cleanup passes. Envelope fixtures explicitly publish at
a boundary before supplying BODY to measure envelope shape independently of
publication latency. Application partition traces retain identical results across
callback sizes; changed startup timing changes their absolute hashes.

The full channel suite passes. STM32 builds successfully. The four 10-second
production-host/simulator phases pass with zero holds/drops (including 200
retriggers). The final four 2-second ThreadSanitizer phases also pass with zero
holds/drops and no reported races. Deterministic refill passes 100 virtual seconds
at eight rates, including 10/20 ms delayed status. Evidence:
`/tmp/channel-owner-suite.log`, `/tmp/channel-owner-board.log`,
`/tmp/channel-owner-final.log`, `/tmp/channel-owner-tsan.log` and
`/tmp/channel-owner-clear-regression`. No hardware was available or flashed.


## Final software completion audit

- Shared implementation: both targets include `app/sources.mk`, use the same
  byte-queue implementation, protocol and renderer, and add only device adapters.
- Nonblocking session ownership: production role CAS loops are removed; storage
  reservation uses the existing command word. The callback performs one role
  publication per operation, with no producer-dependent retry or wait. Same-voice
  console input remains in existing storage; eight voices can share a boundary.
- Startup/replacement correctness: explicit boundary and readiness tests pass;
  current/target status is coherent, old samples remain available during staging,
  clear retires both published and late reserved queues, and upload is excluded
  while commands are pending.
- Stream/cancellation/default-expiry behavior: full channel suite and all six
  host CTests pass. Deterministic refill, real-PTY cancellation, partial writes,
  configured expiry, reopen and existing GUI tests are included. Host transport
  timeout defaults remain zero. No timeout or buffer enlargement was added.
- Runtime evidence: the ownership-refactor 40-second workload reports zero
  holds/drops. Final-revision ThreadSanitizer playback reports zero holds/drops
  and no race diagnostics. The separate clear interleaving regression reproduces
  failure without the cleanup and passes with it. The lifecycle diagnostic also
  passes 100,000 transitions against the actual production ring implementation.
- Build and size: final STM32 build passes. Relative to the pre-ownership build,
  `.text` decreases from 148012 to 147876 bytes; `.data` (1316), `.bss` (71192),
  DMA, VM and attack storage are unchanged. The earlier nine unused HAL-unit
  removals and shared-source release packaging remain in place; all 30 packaging
  tests passed after source-list consolidation.

Host test evidence: `/tmp/channel-owner-host-recheck.log`. The host envelope
fixture now grants actual free credit to pending preparation before refilling
outgoing data, and measures generated envelopes from sample readiness. Its
explicit 100 ms simulated test limit detects a preparation failure; it is not a
production timeout. The protocol boundary test independently verifies the new
publication timing. `git diff --check` passes. Hardware timing is unverified,
as requested software/simulator verification was performed without a board.

## Whole-flow simplification (2026-10-18)

USB now owns script-upload target, length and offset directly; the duplicate
channel-layer transaction state and unused USB text console were removed.
Unused sample-player state and duplicate loaded/frequency flags were removed.
Playback still uses the existing voice renderer, attack bank and stream buffers.
The compiler validates lowered source in memory and passes it to Berry directly,
removing two source-file round trips. The remaining bytecode temporary file is
required by Berry's save API. The card no longer compiles unused vm_source.c.

The 14 compiler fixtures match the previous compiler's acceptance and full output
bytes. Six host tests and 26 compiler/release/flash tests pass. Four 10-second
threaded simulator streaming phases report zero drop/hold counts; four 2-second
simulator ThreadSanitizer phases also pass without race diagnostics. This does
not instrument the host or establish physical USB/DAC timing. STM32 builds with
.text 147540 and .bss 70744 bytes; existing audio/sample/VM storage is unchanged.
Evidence is under /tmp/channel-whole-flow.

New upload-error coverage reveals a pre-existing ownership leak: an invalid FWSC
header causes the backend to clear upload_active before the application abort
handler checks it. That handler then fails to release runtime_owner. The original
snapshot and current production code both fail the regression. A separate
/tmp/channel-whole-flow/candidate copy checks the owning voice instead of the
already-cleared active flag; it passes the upload regression and full channel
suite, including baseline PCM comparisons. This behavior fix is awaiting explicit
agreement under AGENTS.md and has not been applied to production. The new
upload-errors regression therefore currently fails against production.

## MAS sequencing audit (2026-10-18)

The SVN reference remains unchanged; no SVN commit or push was made. The actual
MAS series2.be program passes the existing production-host/PTY streaming tests.
Additional host coverage now exercises all 32 sample slots (eight instruments,
four pitch banks), overlapping starts/releases, finite sample exhaustion,
stop/restart and clear followed immediately by eight script uploads.

The new stop/load case fails on production: clear acknowledges posting the stop,
but begin_voice_script_upload checks the old published active mask before its
pending-clear check. MAS q_cpu_t::apply_patch uses precisely this stop/load order.
A focused protocol test reproduces rejection before the audio boundary. In an
isolated candidate, acquiring the existing runtime ownership before inspecting
pending-clear/activity lets the parser retain BEGIN until the stop completes.
Both the focused regression and the expanded MAS-pattern integration pass with
that candidate, with zero drop/hold counts. No sleep, retry, timeout or queue was
added to production. This additional behavior fix also awaits agreement.

Two unused host members (saved configuration and copied port name) were removed;
all six host CTests pass. ThreadSanitizer mode now instruments the host as well
as the simulator, rather than the simulator alone.

The MAS checkout's older voicebd.h lacks the four optional timeout fields used by
the updated implementation; a syntax check confirms the mismatch. Matching the
header in a temporary copy allows the streamer to compile. Building the actual
MAS test application is not yet verified: its options.h dependency is absent from
this checkout. Focused extracted MAS note_on/note_off/release_keys/channel-mask
methods pass allocation/stealing/repeated-key/bank-selection checks with recorded
board calls; that is not a full MAS application test. Evidence and isolated
sources are in /tmp/channel-mas-audit.

The expanded candidate integration also passes with both processes instrumented
by ThreadSanitizer: all five workloads report zero drop/hold, and neither host.log
nor sim.log contains a race diagnostic. See candidate-tsan.log and its retained
process logs. These results apply to the isolated ownership-fix candidate;
production still fails the new upload regressions until those fixes are approved.

## Applied ownership fixes and interface constraint

After the user's instruction to keep MAS and voicebd.h unchanged, the two tested
ownership fixes were applied only to channel_card/app/voice.cpp. The production
protocol suite passes both new regressions. Earlier optional timeout additions
were removed from voicebd.h; it now has no diff. The host opens transports without
deadlines using the existing configuration interface. No MAS file was changed,
and svn status on /Users/nirajbhatt/175-mainframe is clean.

The MAS voice-board tester now builds and prints its help using its original
header and the updated streamer in /tmp. Its options.h is supplied by this
repository, and linking requires RtMidi and libserialport, not RtAudio. This
supersedes the earlier missing-options build limitation for that tester; it is
still not evidence of running the complete MAS application.

A further caller audit removed 15 uncalled application functions, their header
declarations, an unobserved stream packet counter, and the dead script-stop mask.
The latter removes an atomic exchange from every audio boundary; active clear,
note-off and upload paths are unchanged. Shared hardware/simulator source lists
and existing payload buffers remain intact. No Effect Card files were changed.

Final production verification for this pass: final-suite.log passes protocol,
audio partition/baseline PCM, envelope, transport, PTY and DAC checks. The updated
streamer passes all six host CTests. final-tsan.log passes the four playback loads
and MAS bank/sequence/reload pattern with zero drop/hold counts and no sanitizer
race diagnostics in either process. STM32 and native simulator builds pass;
STM32 .text is 147164 bytes, 376 below the previous cleanup build. No diff remains
in voicebd.h. Full MAS application execution and physical board timing remain
unverified; the goal is not marked complete on these narrower results.

## Remaining signal-path audit

Removed the unused eight-tap interpolation implementation and declarations from
oscillator.cpp/.h after a repository-wide caller check. The active voice path
continues to use its existing two-tap interpolation. Added independent signal
properties to boundary_test.cpp: filter bypass/reset, octave pitch tracking,
constant-wave modulation, graph replacement, cyclic-route rejection and repeated
allocation/retirement across eight voices. No production DSP behavior changed.

signal-suite.log passes the full suite and baseline PCM comparison. The focused
ASan/UBSan executable passes with UBSAN_OPTIONS=halt_on_error=1 and an empty
signal-sanitizers-runtime.log. STM32 and native builds pass; .text is 147156 bytes,
.bss is 70744 bytes. MAS, voicebd.h and Effect Card remain unchanged.

A dry-run of the full MAS make target confirms the remaining build dependency:
../lib/libinfostream-arm64.a is absent (full-mas-prerequisites.log). Matching
headers are also needed. The dependency location has been requested; full MAS
application execution remains unverified, distinct from the passing MAS tester
build, extracted voice-method checks and real-streamer simulator workloads.

## Infostream dependency located

The user supplied http://192.168.1.60/svn/main/. Read-only path searches found
155-Series3Basestation/Library/trunk, whose lib/makefile produces
object.arm64/libinfostream-arm64.a. Exported revision 24998 into
/tmp/channel-mas-audit/infostream and built the library successfully. The previous
infostream blocker is resolved. A full MAS build in a temporary copy now advances
to compilation and fails on missing GLFW/glfw3.h; see full-mas-build.log. The
original SVN checkout remains clean; nothing was committed or pushed.

## Full MAS build and startup investigation after GLFW installation

Installed GLFW 3.5.1 through Homebrew as requested. Full MAS builds successfully
in /tmp/channel-mas-audit/full-mas using the exported infostream library. The
command-line LDLIBS override omits the unused -lrtaudio link dependency; no MAS
Makefile was edited. See full-mas-build-glfw.log and full-mas-link.log.
The temporary integration build uses the current host voicebd.cpp with MAS's
original voicebd.h. The original SVN checkout remains clean, and the workspace
voice_bd/voicebd.h has no diff. Nothing was committed or pushed to SVN.

A test-only dyld adapter redirects hardcoded serial paths to simulator PTYs and
/etc/freshwater assets to temporary copies. Full MAS starts and reaches sound
board initialization, but no screen output appears. Compiled mode was observed
for 120 seconds; a control without the Channel Card also stalls. The documented
interpreter launch mode, with copied legacy sources, likewise produces no screen
within its 20-second observation. These are explicit test watchdogs, not added
application timeouts. Runtime logs and process samples are under runtime/,
runtime-no-card/ and runtime-interpreter/ in /tmp/channel-mas-audit.

Samples locate the compiled P2 wait in WAITP1 and P1 activity in legacy channel
timer processing. Read-only inspection also finds channels_t initializes its time
with double_tod(), whereas process_io supplies double_mono(). This clock-domain
mismatch is a finding, but its responsibility for the startup stall has NOT been
established. No MAS behavioral change was made. Full MAS keyboard playback and
end-to-end audio remain unverified.

The separate current production host-to-simulator test passes one voice, eight
voices, higher-pitch playback, retriggering and MAS bank/sequence/reload workloads,
with zero reported USB drop/hold events (live-build.log). This does not substitute
for full MAS startup/playback verification.

## MAS startup timer cause confirmed by read-only debugger

A fresh run with the current integration binary captured the actual stalled
loop in runtime-timer-count/lldb-counts.txt. channels_t::process had
`counts = -900051997902798` (64-bit long), while c11_q1 accepts a 32-bit int.
The ARM64 disassembly checks w20 and copies the count into w26 for its countdown:
the narrowed value is 1,348,646,962, positive. The snapshot still had
1,238,605,599 iterations remaining, on channel index 7. P1 was inside
mc6840_t::clock calling double_mono; P2 was waiting for P1. This establishes the
mechanism causing the observed startup stall, rather than merely a suspicious
clock mismatch.

The source path is the unchanged MAS channel.h:199 initializer
`channels_t() : selected(0), tod(double_tod())`, followed by masm.cpp:5562 passing
`double_mono()` into channel.cpp:1170 channels_t::process. Its elapsed-clock
calculation feeds channel.cpp:1183 c11_q1(int). The direct proposed correction is
to initialize this time origin with double_mono() as well. That is a MAS behavior
change and is NOT applied under the user's explicit no-MAS-changes constraint.
There may be further startup/playback defects after this one; no claim of full
MAS playback verification is made. Diagnostic processes have been stopped, SVN
status remains clean, and voice_bd/voicebd.h still has no diff.

## Authorized MAS timer fix applied and full-app retest

The user authorized only the timer correction. Changed the original SVN checkout
trunk/mas/channel.h:199 from double_tod() to double_mono(). SVN diff and status
confirm this is the sole changed MAS file and the diff is exactly one line.
voicebd.h and Effect Card remain unchanged; no SVN commit or push was performed.
Copied that header into the temporary integration build and rebuilt successfully
(timer-fix-build.log).

The compiled full MAS now proceeds beyond WAITP1, reads its system disk, emits
TVT diagnostic text, and exits cleanly on SIGINT (runtime-fixed, runtime-xdisplay
and runtime-raw). Samples show P2 in its normal background wait and P1 executing
legacy functions instead of the billion-iteration initial timer loop. This
verifies the reproduced timer stall is resolved, without adding a delay or
changing a buffer.

Playback verification remains incomplete. The full graphical display is blank
under the existing X11 backend on an authenticated Xvfb display, as well as the
initial headless test. Screen-stream capture alone is insufficient: -x disables
the X11 graphics producer, and floating-point graphics do not necessarily emit
the same byte stream. A direct XWD capture also showed a blank display. A raw
PTY was used to preserve keyboard carriage returns, and P1 plus Enter did not
produce a usable page. Compiled startup reports Q256 uninitialised reads at
5f33/5f3b; these messages alone do not establish the cause. A sequential run with
the Channel Card disabled reproduces the same startup output
(runtime-fixed-control), so the remaining issue is not specific to the streaming
connection. Interpreter mode instead aborts at q_cpu_t::jump, masm.cpp:3747,
with i != jump_tokens.right.end() (runtime-interpreted-fixed/mas.log). Neither new
MAS behavior has been changed. Full keyboard playback and sequencing cannot yet
be claimed verified, and no broader MAS fix is authorized.


### Full MAS display and playback investigation (19 October 2026)

All display/token candidates remain **test-copy only**, as explicitly requested.
The original SVN MAS checkout still differs only in the approved channel.h
wall-clock-to-monotonic timer initialisation. voicebd.h matches the original;
no Effect Card changes or SVN commits were made.

A controlled X11 exposure test confirmed the blank-window defect: cover an
already rendered MAS page with another window, then destroy that window. The
baseline becomes blank because ExposureMask is disabled and exposure events
are not handled. In the temporary candidate, XInitThreads is called before
Xlib use and P2 redraws the existing image on Expose. Before/after XWD pixel
payloads were identical (241104 nonzero component bytes), without keyboard
input. Evidence: /tmp/channel-mas-audit/runtime-candidate/{before,after}.xwd.

The interpreter's shared jump_tokens bimap is lazily mutated by get_pc on both
CPU threads with no synchronisation. A baseline crash was in Boost tree
insertion/rebalancing; another failed the reverse-token lookup in jump after
sub_return. Preallocating opcode and return-site tokens is NOT yet a verified
fix. A test-only frozen-table assertion caught missing tokens at module 32,
line 1188 (FAST page-cmdchk), then module 33, line 1125 (a SET directive).
Preallocating all source lines exceeds the 16-bit token space. No candidate
has been promoted; runtime lookup coverage remains unresolved. Logs:
runtime-frozen-detail/mas.log and runtime-playverify/mas.log under the same
/tmp/channel-mas-audit evidence directory.

Compiled full MAS playback now runs through the current host streamer into
the shared Channel Card application on PTYs, with a test-only PortAudio
replacement driving the production callback. A test observer counts nonzero
PCM, underruns and dropped blocks after the callback stops. This verifies
software rendering, not physical USB/DAC timing or speaker output.

The clean runtime-library run selected 03 Best of US.hfe, loaded TEENTOWN.IN
and its eight voices, then used Page R LOAD,TEENTOWN and PLA. The bar counter
advanced and MAS emitted musical keyboard frames. STOP followed by LOAD,DEMO
and PLA loaded and played a second sequence. Both processes exited 0. Across
3326720 rendered frames the observer counted 3211524 nonzero sample values,
peak 985113997, **0 underruns and 0 dropped blocks**. Screenshots, commands and
logs are in runtime-library. Bundled disk images were extracted only beneath
/tmp/channel-mas-audit/full-mas/library (463 VC and 41 RS files).

An earlier keyboard/chord/retrigger test produced PCM but recorded 6710
sample-level underruns. It included an LLDB pause of the simulator while MAS
continued running, so it is not a clean timing test and must not be substituted
for the uninterrupted result. The exact cause of those misses is not yet
isolated. Evidence: runtime-compiled-play.

LIB was rejected on the hosted Page 2 and native Pages 3/R in this checkout;
clarification of the intended LIB interface is pending. Native Page R LOAD
and PLA work as described above. Full interpreter playback remains unverified.


Follow-up: ThreadSanitizer on the actual full MAS masm.cpp reproduced the token
race: a main-thread read and thread T8 write both in module_t::get_pc at
masm.cpp:3730, inside the shared Boost bimap insertion path. The run halted on
the first race; this is not a clean sanitizer pass. Evidence:
/tmp/channel-mas-audit/runtime-baseline-tsan-confirm/mas.log. This confirms the
concurrent token-map access suspected from the assertion/tree-corruption
crashes. The normal temporary binary is being restored to the display-only
candidate; failed token-preallocation experiments remain separately preserved
as masm-candidate-frozen-pseudo.cpp and mas-candidate-diagnostic. Interpreter
mode is still unsafe and has not been declared fixed.

The display-only temporary binary rebuilt successfully (display-only-build.log).
The reviewable display-only change is /tmp/channel-mas-audit/display-only.patch.
No running test MAS/simulator or test display is intentionally retained.


### Uninterrupted full-MAS keyboard retest (19 October 2026)

Repeated the single-note, eight-note chord, note releases, MIDI keys 60/72/84
and same-note retrigger workload without a debugger attached. Native MAS
commands P2, CreateVoice, Nphony A 8 and Load test-tone.wav each completed
successfully. The test injected the same three-byte musical keyboard frames
that MAS's MIDI translation creates, through its production keyboard input.
Both MAS and the software-clock simulator exited 0. Measurement: 1348736
rendered frames, 398618 nonzero sample values, peak 1471647768, **0 underruns,
0 dropped blocks**. Evidence: /tmp/channel-mas-audit/runtime-clean-chords.
This establishes a clean pass for that workload; it does not alone prove the
LLDB pause caused every miss in the earlier interrupted run.


### Native PortAudio full-MAS test exposes remaining underruns

The clean software-clock passes do not establish native callback timing.
A full MAS TEENTOWN.IN / Page R TEENTOWN then DEMO load/play/stop test was run
through real PortAudio/CoreAudio, with a test-only wrapper around the existing
production callback. No audio buffers or timing policy were changed. The test
observer counted 1657344 rendered frames, 2509746 nonzero sample values, peak
1075990897, callback flags 0, **17551 renderer sample-miss events**, and zero
dropped BODY blocks. Both processes exited 0. STOP settled musical input at
1177 frames. Evidence: /tmp/channel-mas-audit/runtime-native-sequence.

This contradicts any broad conclusion that all full-MAS streaming is underrun
free. Device underflow flags and stream sample starvation are distinct: flags
were zero while the renderer missed samples. Root cause is not established;
a follow-up test records callback timestamps and per-callback miss increments
in fixed test memory, written only after callbacks stop. No production change
or buffer/timeout increase has been made in response.


Native timing follow-up: runtime-native-trace completed with 15060 callbacks,
1927680 frames, callback flags 0, 16526 sample misses, zero dropped blocks.
Crucially, **all misses are in the final 28 callbacks**, beginning 40.10959 s
from the first callback and ending 40.17381 s. The harness requested shutdown
at 40 s. There were no sample misses during the preceding sequence playback.
The trace therefore shifts investigation to shutdown ordering rather than
steady-state native rendering. Pa_StopStream drains output while the MAS host
has already stopped; whether the host's best-effort clear was delivered and
processed remains to be measured. Do not yet claim a proven shutdown fix or
attribute every earlier native miss to the same cause. Trace:
/tmp/channel-mas-audit/native-callbacks.csv. Callback intervals show CoreAudio
batches (11295 intervals below 0.2 ms, median 0.094 ms), but those batches alone
did not cause sample misses during this playback run.


### Shutdown cause confirmed; disconnect candidate remains test-only

The correlated native test proves sample starvation starts after MAS closes
both PTYs, before Pa_StopStream begins. In runtime-shutdown-trace, MAS SIGINT
was at 1626579053480000 ns; USB/RS485 disconnects at 1626579054505000 /
1626579054541000 ns; the first miss at 1626579100264000 ns; the last miss at
1626579164221000 ns; Pa_StopStream began at 1626579172188000 ns. Thus output
drain in Pa_StopStream was not the source of those misses.

A test-only write interposer then recorded all 8 c:clear bytes successfully
written at 1626625205776000 ns; the simulator observed RS485 hangup at
1626625205799000 ns without receiving that clear. An independent macOS PTY
reproduction, with both raw and default termios, writes 8 bytes then closes
the slave; master poll reports POLLIN|POLLHUP and read returns EOF, not the
queued command. The best-effort nonblocking clear is therefore not a remote
acknowledgement. Audio continues after its data producer exits and starves.
Evidence: runtime-clear-write, runtime-shutdown-trace and their callback CSVs.

A temporary prototype stops voices via request_stop_all_voices when a USB
connection-generation change is observed while disconnected. The first
simulator-only experiment passed; the final experiment places the check in
shared app/usb.cpp process_usb_messages, using the existing audio ownership
handoff and no new queue, buffer, wait or timeout. The latter full MAS native
run rendered 871936 frames / 925728 nonzero sample values with callback flags
0, underruns 0, dropped blocks 0, and both processes exited 0. Evidence:
runtime-shared-disconnect and shared-disconnect-callbacks.csv.

This is an explicit proposed disconnect policy, not a production fix yet.
Approval was requested because it changes disconnected playback from repeating
buffered samples to stopping all voices at the next audio boundary. The source
checkout remains unchanged; candidate source is
/tmp/channel-mas-audit/capture-tests/usb_observer.cpp. Original MAS remains
limited to its approved timer fix; voicebd.h and Effect Card are untouched.

The temporary shared candidate also passed focused assertions for readiness
pause without a generation change, disconnected generation change, stop at
the audio boundary, and reconnection without a stale stop request, followed
by the existing protocol regression assertions. Build/result evidence:
shared-disconnect-protocol-build.log and shared-disconnect-protocol.log.


### STM32 verification of the pending shared disconnect candidate

An isolated Make wrapper compiled the exact temporary usb.cpp candidate with
production STM32H725/Cortex-M7 flags and linked it against the current hardware
objects and linker script. The candidate differs from production by one line
inside process_usb_messages: on a changed connection generation, request the
existing all-voice stop if the transport is disconnected. Build succeeded.
GNU size comparison: text 167148 -> 167164 (+16 bytes), data 1316 unchanged,
bss 321128 unchanged. These are GNU aggregate sizes, not the narrower section
figures quoted in earlier reports. No new RAM or buffer was allocated. Both
targets already list app/usb.cpp in app/sources.mk, so no adapter duplication
or additional production file is needed. No hardware execution is claimed.

Evidence: /tmp/channel-mas-audit/stm32-disconnect-build.log,
stm32-disconnect.mk, stm32-disconnect.elf. The candidate ELF and map are under
/tmp only; the production firmware was not replaced by the candidate.


### Interpreter token-space audit (test copy only)

A diagnostic inspected the actual expanded MAS source before either CPU
thread started, then exited successfully. Counts: 192549 total source lines,
37765 empty, 45235 normal opcodes, 92055 ignored directives (67005 EQU, 23660
SET, 823 PSCT, 567 DSCT), and 9583 tokens already assigned to symbols.
There are also 77 FAST and 94 SCALL pseudo-instructions. This quantifies why
preassigning every possible source-line token cannot fit the existing 16-bit
representation. A subset of normal opcodes is insufficient: previous frozen
runtime tests reached FAST and SET lines absent from that subset.

mc6809_t::interrupt saves get_pc(), and ignored directives still advance the
interpreter's cycle count. Normalising saved PCs past them would change exact
resume positions, traces and cycle accounting; it is not established as a
behavior-preserving repair. No such normalisation was applied. The token
race is confirmed, but a minimal correct interpreter repair remains open.
Evidence: /tmp/channel-mas-audit/runtime-token-space/mas.log,
masm-token-space-audit.cpp and mas-token-space-audit. The normal temporary MAS
sources have been restored to the previously tested display-only candidate.

## Approved shared disconnect stop applied — 19 October 2026

The user approved the shared disconnect stop. `process_usb_messages()` now calls
`request_stop_all_voices()` when the USB connection generation changes and the
link is disconnected. The existing audio handoff performs the stop. A readiness
pause without a generation change does not stop playback; reconnection does not
request another stop. No queue, buffer, blocking operation, or timeout was added.

The protocol regression verifies those three cases and that rendering consumes
the stop request and retires the active voice. The full Channel Card host suite
passes, including transport concurrency, serial failure paths, PTY reconnect,
upload errors, envelope/PCM partition checks, boundaries, and DAC tracing. Both
the STM32 board and simulator builds pass; no hardware execution was possible.

Full MAS compiled-mode verification used the production application and serial
adapter sources, with a test-only native audio observer. Commands were `P2`,
`Load TEENTOWN.IN`, `PR`, `LOAD,TEENTOWN`, and `PLA`, followed by host exit during
playback. Result: 872448 frames, 932832 nonzero sample values, peak 869620246,
callback flags 0, underruns 0, dropped blocks 0. Evidence is under
`/tmp/channel-mas-audit/runtime-production-disconnect`, with callback timestamps
in `production-disconnect-callbacks.csv` and build/test logs named
`disconnect-production-*.log`. This verifies this shutdown failure, not every
possible host stall or hardware timing condition.

No additional MAS changes were made. The separate interpreter jump-token race
remains unresolved; the display fix remains confined to the MAS test copy.
`voicebd.h` and Effect Card are unchanged, and nothing was committed to SVN.
