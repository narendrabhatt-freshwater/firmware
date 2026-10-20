# Coding and debugging rules

## Code style

- Use the supplied `channel-rewrite/channel.cpp` and `channel-rewrite/main.cpp`
  as the current style reference for new and edited application code. Match the
  compact functions and control flow; do not reformat unrelated existing code.
- Use four spaces per indentation level; configure tabs to insert four spaces.
- Put opening braces on the next line for functions. For control-flow blocks,
  use `if (...) {`, `for (...) {`, and `} else if (...) {` as in the rewrite.
  A short, clear single-statement condition may stay on one line, for example
  `if (n < 0) continue;`. A trivial function body may also fit on one line.
  This replaces the earlier requirement for next-line control-flow braces.
- Keep related statements together. Do not add blank lines between every
  declaration, call, or branch, or expand simple statements across excessive
  lines. Use blank lines to separate logical sections.
- Put a descriptive banner comment above every function, using the style
  `/* ---- generate a sine wave buffer ---------------------------------------- */`.
  Do not add inline comments for now.
- Declare one variable per statement, on its own line. Never combine variable
  declarations with commas.
- Prefer parenthesized direct initialization for simple values and loop
  counters, as requested by the user. For example:

  ```cpp
  int usb_client(-1);
  int rs485_client(-1);
  int n(0);
  bool echo(false);
  for (int i(0); i < SPB; ++i) {
      generate_sample(i);
  }
  ```

  Keep braces where they serve a purpose, such as `fd_set ready{}` for zero
  initialization or byte-vector initializer lists. Do not use `int n()` to
  initialize a variable: that syntax declares a function.
- Prefer trailing `const` and references attached to the type, for example
  `int const n(...)`, `string const& command`, and
  `vector<uint8_t> const& bytes`.
- In implementation files that already use `using namespace std;`, use the
  short names consistently where unambiguous (`string`, `vector`, `cerr`).
  Do not introduce `using namespace std;` in headers.
- Prefer direct free functions and simple file-local state. Do not use the
  PImpl (pointer-to-implementation) pattern, hidden `Impl` objects, or extra
  class/wrapper layers for this application flow.
- Use a small helper when it removes repeated work, as `rs485_response()` does
  for response conversion and line endings. Keep command branches short rather
  than rebuilding the same response machinery in each branch. Keep local
  protocol defines in the implementation until they need to be shared.
- Treat the rewrite's protocol, echo, and transport changes as behavior, not
  formatting rules. Do not copy those changes into other code as part of a
  style cleanup; the behavior-change and retained-I/O rules below still apply.

## Makefile style and compiler warnings

- Use `../175-mainframe/mas/makefile` and `channel-rewrite/makefile` as the
  Makefile style references. Preserve the Freshwater header, align variable
  assignments and continuation lists, and use `# ---- section ...` banners
  for compiler settings, objects, targets, build rules, cleanup, and dependencies.
  Recipe commands still require literal tabs; the four-space C++ rule does not
  apply to Makefile recipe prefixes.
- Keep the Makefile small and specific to its target. Preserve its language
  standard, optimization, platform libraries, output paths, dependency tracking,
  and supported overrides rather than copying unrelated MAS build machinery.
- In `channel-rewrite/`, use `OBJDIR = object.$(HOST_ARCH)` with the host
  architecture from `uname -m`, following MAS's architecture-specific object
  directories. Objects and dependency files go in that directory (for example,
  `object.arm64/`). Use `BINDIR = bin` for the `bin/simulator` executable;
  `clean` removes the active object directory and that executable.
- Leave `WARNING_FLAGS ?=` empty by default, following MAS. Do not add or
  reintroduce `-Werror`, `-Werror=...`, `-Wall`, `-Wextra`, `-pedantic`, or other
  warning-policy flags in builds, tests, or validation commands unless the user
  explicitly requests them. Do not add blanket warning suppression either.
  Compiler-default warnings may still appear; do not hide diagnostic output or
  ignore compiler failures. Existing unrelated build files are not an invitation
  to change their flags during another task.

## Simulator flow and simplicity

- Do not introduce atomics (`std::atomic` or `__atomic_*`) unless explicitly
  requested. Keep the simulator's write index owned by the main loop.
- Keep `channel/main.cpp` in its original form; only the application initialization
  include/call and the approved small I/O fixes are allowed: correct USB/RS485
  routing, clear delivered input chunks, and fill the serial/USB output stubs.
  Keep application buffering, parsing and reply processing in `channel.cpp`.
  The approved audio setup also selects the device's recommended low output
  latency and gives the macOS simulator main thread real-time scheduling through
  `setup_audio_thread()`, with Mach headers and code guarded by `__APPLE__`.
  The USB PTY also uses packet mode to report the client's port-opening flush;
  forward that connection event to the application and strip the status byte
  from data packets. USB session state is cleared inside `channel.cpp`.
- The shared application lives flat in `channel/`; Berry runtime/vendor files
  stay in `channel/berry_runtime/`. Board-specific code stays in `channel_card/`.
- Initialize the application once, then prefill `BUF_COUNT - 1` buffers before
  starting playback. Keep the current 30-slot ring and 48-sample renderer.
- Preserve variable callback sizes and partial-buffer consumption. The callback
  owns its read index and offset; the main loop owns the write index.
- The callback copies existing samples and posts one refill token per consumed
  buffer through the nonblocking request pipe. It never waits for generation.
- The request pipe carries audio-refill tokens only. Platform I/O only moves
  bytes. Inside `channel.cpp`, receive functions buffer input, process complete
  requests and send available replies without waiting for audio generation.
  `generate_samples()` advances playback and services pending application work.
  Parsers process buffered messages directly; output bookkeeping only removes
  transmitted bytes. Do not add main-loop wakeup callbacks or another-pass flags.
  Do not add application scheduling helpers to `main.cpp` or desktop-only
  compilation guards to this application path.
- Keep Berry, parsing, and mixing on the main thread. Protocol deadlines use
  platform milliseconds; generated audio advances the application's sample clock.
- Preserve the original `channel.h` interface and its `void` functions taking
  const byte vectors. Only the matching `write_usb()` declaration may be added.
- Keep DAC calibration, output packing and DMA code in `channel_card/`.
- Retain unaccepted input and unsent output. Pass only input that fits the
  application buffer and remove only bytes written from output queues.
  Keep USB and RS485 independent.
- Do not add another ready/completion pipe or a pending-refill counter.
- Plain `make` in `channel/` uses lowercase `makefile` and builds `bin/simulator`.
- Assume simulator setup and I/O succeed. Avoid defensive error checks, retry
  wrappers, initialization flags, and unreachable cleanup. Keep conditions
  needed for normal flow, such as pending audio work and available serial data.

## Establish the root cause before changing behavior

- Reproduce the failure and trace the actual cause using code, logs, measurements,
  or a focused test. Clearly distinguish confirmed findings from hypotheses.
- Fix the underlying defect. Do not hide symptoms with arbitrary delays, larger
  timeouts or buffers, retries, dropped errors, silent fallbacks, forced resets,
  or reconnect requirements. A parameter change needs evidence that the parameter
  itself violates the actual timing, capacity, or protocol requirement.
- Before changing production behavior, explain the problem, supporting evidence,
  proposed fix, and consequences to the user and agree on the solution. Permission
  to investigate or test is not permission to introduce a new recovery policy or
  workaround. Existing approval for a specific fix remains valid.
- If testing uncovers another defect, report it and discuss the solution before
  implementing another behavior change. Keep diagnostic experiments separate
  from production code and binaries.
- If the root cause is still unknown, say so and continue investigating. Do not
  present a plausible explanation or a passing workaround as a confirmed fix.
- Verify that the fix addresses the reproduced failure and test relevant adjacent
  behavior and failure paths. Report exactly what passed and what remains
  untested; do not claim that all regressions have been ruled out.
- If the user explicitly requests a temporary mitigation, identify it as such
  and keep the unresolved root cause visible.
