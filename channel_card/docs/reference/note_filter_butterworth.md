# Channel Card — Per-voice 4-pole Butterworth LPF

Sources: [`note_filter.c`](../../channel_card/Core/Src/filters/note_filter.c),
[`note_filter.h`](../../channel_card/Core/Inc/filters/note_filter.h),
kernel [`butterworth_four_pole.c`](../../channel_card/Core/Src/filters/butterworth_four_pole.c),
wired from [`note_bank.c`](../../channel_card/Core/Src/audio/note_bank.c).

This document explains **why** the filter is built this way, the **structure**
used in firmware, and how to **verify** it on a scope against theory.

---

## 1. Goal

Each of the 8 note-bank voices (`n0`–`n7`) gets its own **4-pole
Butterworth low-pass** before the voices are mixed onto DAC CH1.

```text
  voice 0:  source → × amp/env → 4-pole LPF → ─┐
  ...                                          ├─ sum → saturate Q31 → I2S CH1
  voice 7:  source → × amp/env → 4-pole LPF → ─┘
```

The voice source is either the SAMPLE path (attack head + streamed
sustain) or the global DDS shape: `s` (sine), `p <0.1..0.9>` (pulse
duty), `t <0.1..0.9>` (triangle asymmetry). On a pure sine the LPF
mostly changes level (and phase); pulse/tri give harmonics for filter
sweeps.

The digital filter is a **4-pole Butterworth LPF** (reference DF4
`four_pole_filter` direct-form algorithm). `cutoff` sets the corner; **20000 Hz**
(or console **`0`**) is transparent bypass. Optional console **`q`** is DF4
**`g`** (**0.5..10**, default **1.0** ≈ Butterworth); **higher q → more peaking
near fc** — not RBJ biquad Q, and not the analog VCF's resonance CV. HP from
the same reference kernel is implemented but **not wired** to a console
command in this build (always LP).

Why per-voice (not one filter after the mix): independent cutoff per
note. The analog VCF/SCF on the board still filter the **summed** path and
are complementary, not a substitute.

**Floating point:** coefficients, delays, and per-sample filtering use
`double` (H725 FPU). Q31 conversion happens only at the
`NoteFilter_Process` edges. This is an intentional exception to the
usual no-float-on-the-hot-path rule, confined to this module.

---

## 2. Structure choices

| Choice                            | Reason                                                   |
| --------------------------------- | -------------------------------------------------------- |
| Reference DF4 (`coef[9]`, `d[4]`) | Match the `four_pole_filter` reference bit-for-algorithm |
| `g` / console `q` (default 1.0)   | Shape param; 1.0 ≈ Butterworth; higher → more peak       |
| `double` hot path                 | FPU present; stay faithful to reference                  |
| LPF only in v1                    | Ship LPF first; HP init kept in kernel for later         |
| Bypass at 20 kHz                  | Default = transparent; matches pre-filter bank behaviour |
| Reset delays on cutoff/q change   | Avoids clicks from stale state                           |

---

## 3. Design (what `ButterFourPole_InitLowPass` does)

One 4th-order direct-form section per voice — five feedforward and four
feedback coefficients (`coef[9]`) with four delay states (`d[4]`). No
second-order-section cascade and no fixed-point coefficient
quantization; everything stays `double`.

Sample rate is `AUDIO_SAMPLE_RATE_HZ` (currently 48 kHz). The cutoff is
first converted to a digital frequency:

\[
\omega = 2\pi f_c / f_s
\]

(`ButterFourPole_CutoffToOmega`, clamped to \([0,\ 0.999 \cdot f_s/2]\)).

The shape parameter `g` (console `q`) enters through

\[
k = \frac{4g - 3}{g + 1}
\]

so `g = 1.0` gives `k = 0.25` — the classical maximally-flat corner —
and larger `g` raises the feedback term, peaking the response near
\(f_c\). The bilinear-style prewarp uses `tan(ω/2)`; the resulting
binomial products of \(p = 1 + a\) and \(q = 1 - a\) form the five
numerator taps (`p·{1,4,6,4,1}`) and four denominator taps.

### Hot-path difference equation (direct form, 4 delays)

```c
out  = coef[0]*in + d[0];
d[0] = coef[1]*in + coef[5]*out + d[1];
d[1] = coef[2]*in + coef[6]*out + d[2];
d[2] = coef[3]*in + coef[7]*out + d[3];
d[3] = coef[4]*in + coef[8]*out;
```

At \(f_c \ge 20000\ \mathrm{Hz}\) (or 0) `note_filter.c` bypasses the
kernel and returns the input unchanged.

Redesign (cutoff, q, pitch-track) is **cold-path only** — triggered by
`n*` / `f*` / `fk*` console commands, never per sample. Delay lines are
cleared on redesign to avoid clicks from stale state.

---

## 4. Console API

| Command                                   | Meaning                                                                  |
| ----------------------------------------- | ------------------------------------------------------------------------ |
| `f0`…`f7` `<Hz>` `[q]` / `f` `<Hz>` `[q]` | Set **base** cutoff at C4 (20…20000); optional **q** = DF4 g **0.5..10** |
| `f0`…`f7` / bare `f`                      | Query one voice / all eight (effective fc, q, k)                         |
| `fk0`…`fk7` `[k]` / `fk` `[k]`            | Filter pitch-track **k** (0…10, default 0); bare = query                 |
| `0` or `20000`                            | Bypass (transparent; stored/reported as 20000)                           |

Default **q = 1.0** (Butterworth-like). Higher q → more peak near fc. Replies:
`ok: …` / `err: …` (never silent clamp).

### 4.1 Pitch-track (CMI-style key follow)

\(f_{\text{base}}\) is the cutoff programmed with `f0` — the corner **at C4**,
not the key frequency. Pitch-track amount \(k\) (console `fk`) scales it:

\[
f_c = f_{\text{base}} \cdot 2^{k\, n / 12}
= f_{\text{base}} \cdot (f_{\text{note}} / C4)^k
\]

with \(n = 12\log_2(f_{\text{note}}/C4)\) and \(C4 = 261.625565\,\mathrm{Hz}\)
\(k = 0\) → absolute;
\(k = 1\) → full 1:1 key follow (octave up doubles \(f_c\)).

Track never flips bypass; overflow clamps to just below 20 kHz.

```text
f0 300
fk0 1
n0 261.63 1          # fc ≈ 300 Hz (at C4)
n0 523.25 1          # fc ≈ 600 Hz (C5)
```

Requires firmware that includes `note_filter.c`. Old images reply with the
generic unknown-command error (type `h` for the live list).

---

## 5. Scope verification (sine test)

A **sine in → LPF → sine out** at the **same frequency**. The output is
not a reshaped waveform — verify with the **amplitude ratio**.

### 5.1 Procedure

```text
n off
n0
g 1 0
n0 1000
f0 20000                # reference (bypass)
# measure Vpp_ref

f0 300            # engage LPF
# measure Vpp_filt  (same timebase / trigger)
```

Period must stay **~1.00 ms** (1000 Hz). Only **height** changes.

### 5.2 Expected gain (analog prototype, good check at these fc ≪ fs/2)

For `g = 1.0`, the classical order-4 Butterworth magnitude is a close
approximation:

\[
G = |H(f)| = \frac{1}{\sqrt{1+(f/f_c)^8}}, \qquad
\frac{V_{pp,filt}}{V_{pp,ref}} \approx G
\]

For **\(f = 1000\,\mathrm{Hz}\)** (primary lab case):

|   \(f_c\) (Hz) | \((f/f_c)^8\) | \(G\) (linear) |  \(G\) (dB) | Scope               |
| -------------: | ------------: | -------------: | ----------: | ------------------- |
| 20000 (bypass) |             — |           1.00 |           0 | full height         |
|           5000 |       ~2.6e−6 |         ≈ 1.00 |         ≈ 0 | almost unchanged    |
|           2000 |         1/256 |        ≈ 0.062 |       ≈ −24 | ~1/16 height        |
|           1000 |             1 |      **0.707** |      **−3** | ~70.7% of ref       |
|            500 |           256 |        ≈ 0.062 |       ≈ −24 | ~1/16 height        |
|            300 |        ≈ 1520 |   ≈ **0.0256** | ≈ **−31.8** | ~1/39 height        |
|            200 |        390625 |       ≈ 0.0016 |       ≈ −56 | nearly flat / noise |

*(The digital response tracks the analog prototype closely because
\(f, f_c \ll 48\,\mathrm{kHz}\). ±10–15% on the measured ratio is a pass
for bring-up.)*

### 5.3 What the check looks like on a scope

```text
Bypass (fc=20k)          fc=300 Hz, f=1kHz
  ┌─┐   ┌─┐                ┌┐     ┌┐
  │ │   │ │     same T     ││     ││     same period (~1 ms)
  │ │   │ │     ≈1 ms      ││     ││     much smaller Vpp
──┘ └───┘ └──            ──┘└─────┘└──
```

Frequency counter / period cursors: **unchanged**. Peak-to-peak: **down**.

If a measured ratio disagrees by more than ~15%: old firmware without
the filter, probing the analog VCF path instead of bypass, another host
driving the note bank concurrently, or gain/scale too low for a stable
trigger.

### 5.4 Harmonic demo

Use `p 0.5` or `t 0.5` then sweep `f0` for an audible/filterable
harmonic demo. Sine (`s`) remains the primary amplitude-ratio test.

---

## 6. Files / integration

| File                                       | Role                                                           |
| ------------------------------------------ | -------------------------------------------------------------- |
| `Core/Src/filters/butterworth_four_pole.c` | DF4 design + process kernel (LP + HP init, `double` state)     |
| `Core/Inc/filters/butterworth_four_pole.h` | Kernel API                                                     |
| `Core/Src/filters/note_filter.c`           | Per-voice base/effective cutoff, pitch-k, bypass/q + Q31 edges |
| `Core/Inc/filters/note_filter.h`           | Public voice API                                               |
| `Core/Src/audio/note_bank.c`               | Calls `NoteFilter_Process` after amp; resets on note-off       |
| `Core/Src/console/channel_console.c`       | `f0`…`f7` / `f` with optional q; init bypass + q=1.0           |
| Top-level `CMakeLists.txt`                 | Registers filter + audio sources under `Core/Src/<domain>/`    |

---

## 7. Related

- Analog wet path (VCF/SCF): [`channel_card/README.md`](../../channel_card/README.md)
- Wire protocol (`f` / `fk` commands): [`protocol.md`](../protocol.md)
