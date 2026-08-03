# Channel Card — Per-voice 4-pole Butterworth LPF

Status: implemented (integrated on `develop`)  
Sources: [`note_filter.c`](../apps/channel_card/Core/Src/filters/note_filter.c),
[`note_filter.h`](../apps/channel_card/Core/Inc/filters/note_filter.h),
kernel [`butterworth_four_pole.c`](../apps/channel_card/Core/Src/filters/butterworth_four_pole.c),
wired from [`note_bank.c`](../apps/channel_card/Core/Src/audio/note_bank.c).

This document explains **why** the filter is built this way, the **formulas**
used in firmware, and how to **verify** it on a scope against theory.

---

## 1. Goal

Each of the 16 note-bank oscillators (N0–NF) gets its own **4-pole
Butterworth low-pass** before the voices are mixed onto DAC CH1.

```text
  voice i:  DDS osc → × amp → 4-pole LPF → ─┐
  ...                                       ├─ sum → saturate Q31 → I2S CH1
  voice 15: DDS osc → × amp → 4-pole LPF → ─┘
```

Oscillator shape is global for N0–NF: `s` (sine), `p <0.1..0.9>` (pulse
duty), `t <0.1..0.9>` (triangle asymmetry). On a pure sine the LPF mostly
changes level (and phase); pulse/tri give harmonics for filter sweeps.

The digital filter is a **4-pole Butterworth LPF** (boss `four_pole_filter_t`
direct-form algorithm). `cutoff` sets the corner; **20000 Hz** (or console
**`0`**) is transparent bypass. Optional console **`q`** is boss DF4 **`g`**
(**0.5..10**, default **1.0** ≈ Butterworth); **higher q → more peaking near
fc** — not RBJ biquad Q, and not analog VCF resonance (`dc 4`). HP/BP from that
reference are **not wired yet** (no `pass` console command in this build).

Why per-oscillator (not one filter after the mix): independent cutoff per
note. The analog VCF/SCF on the board still filter the **summed** path and
are complementary, not a substitute.

**Floating point:** coeffs, delays, and per-sample filtering use `double`
(H725 FPU). Q31 convert only at `NoteFilter_Process` edges. Intentional
exception to the usual “no float on the audio hot path” rule for this module.

---

## 2. How we chose the structure

| Choice                          | Reason                                                       |
| ------------------------------- | ------------------------------------------------------------ |
| Boss DF4 (`coef[9]`, `d[4]`)    | Match provided `butterworth.cpp` reference bit-for-algorithm |
| `g` / console `q` (default 1.0) | Boss shape param; 1.0 ≈ Butterworth; higher → more peak      |
| `double` hot path               | FPU present; stay faithful to reference                      |
| LPF only in v1                  | Boss: implement LPF first; HP init kept for later            |
| Bypass at 20 kHz                | Default = transparent; matches pre-filter bank behaviour     |
| Reset delays on cutoff/q change | Avoids clicks from stale state                               |

---

## 3. Analog prototype (where the Q values come from)

An **order-\(n\) Butterworth** low-pass has magnitude

\[
|H(j\omega)| = \frac{1}{\sqrt{1 + (\omega/\omega_c)^{2n}}}
\]

For \(n = 4\):

\[
|H(f)| = \frac{1}{\sqrt{1 + (f/f_c)^{8}}}
\]

By definition, at \(f = f_c\): \(|H| = 1/\sqrt{2}\) (**−3 dB**), for any order.

Poles lie equally spaced on a semicircle in the left half-plane. Cascading
as **quadratic factors** gives two sections, each a 2nd-order LPF with
quality factor

\[
Q_k = \frac{1}{2\sin\!\bigl((2k-1)\,\pi/(2n)\bigr)}, \quad k = 1,2,\ \ n=4
\]

| \(k\) | Angle      | \(Q_k\)                                          |
| ----- | ---------- | ------------------------------------------------ |
| 1     | \(\pi/8\)  | \(1/(2\sin(\pi/8)) \approx 1.3065629648763766\)  |
| 2     | \(3\pi/8\) | \(1/(2\sin(3\pi/8)) \approx 0.5411961001461969\) |

Firmware stores them **lower Q first**:

```c
static const double k_butter_q[2] = {
    0.5411961001461969, /* k=2 */
    1.3065629648763766, /* k=1 */
};
```

---

## 4. Digital design (what `NoteFilter_DesignSos` does)

Sample rate \(f_s = 48000\). For each section with cutoff \(f_c\) and quality \(Q\):

### 4.1 Normalized digital frequency

\[
\omega_0 = 2\pi\, f_c / f_s
\]

### 4.2 RBJ low-pass intermediate (`alpha`)

\[
\alpha = \frac{\sin\omega_0}{2Q}
\]

### 4.3 Unnormalized biquad (RBJ cookbook LPF)

\[
\begin{aligned}
b_0 &= (1 - \cos\omega_0)/2 \\
b_1 &= 1 - \cos\omega_0 \\
b_2 &= (1 - \cos\omega_0)/2 \\
a_0 &= 1 + \alpha \\
a_1 &= -2\cos\omega_0 \\
a_2 &= 1 - \alpha
\end{aligned}
\]

### 4.4 Normalize by \(a_0\) (stored coeffs)

\[
b_i' = b_i/a_0,\quad a_1' = a_1/a_0,\quad a_2' = a_2/a_0
\]

### 4.5 Quantize to Q28 for the hot path

\[
\text{coeff}_{Q28} = \mathrm{round}(x \cdot 2^{28})
\]

Q28 leaves headroom for \(|a_1'| \approx 2\).

---

## 5. Hot-path difference equation (Transposed Direct Form II)

Each SOS uses state \(z_1, z_2\):

\[
\begin{aligned}
y &= b_0'\,x + z_1 \\
z_1 &\leftarrow b_1'\,x - a_1'\,y + z_2 \\
z_2 &\leftarrow b_2'\,x - a_2'\,y
\end{aligned}
\]

Implemented with `int64_t` MAC + `>> 28`, then saturate to Q31. Two SOS
per voice → **4 poles**.

At \(f_c \ge 20000\,\mathrm{Hz}\) the process function **returns `x` unchanged**
(bypass).

---

## 6. Console API

| Command                                   | Meaning                                                                         |
| ----------------------------------------- | ------------------------------------------------------------------------------- |
| `f0`…`f7` `<Hz>` `[q]` / `f` `<Hz>` `[q]` | Set cutoff (20…20000); optional **q** = DF4 g **0.5..10** (omit → keep current) |
| `f0`…`f7` / bare `f`                      | Query one voice / all eight (includes q)                                        |
| `0` or `20000`                            | Bypass (transparent; stored/reported as 20000)                                  |

Default **q = 1.0** (Butterworth-like). Higher q → more peak near fc. Replies:
`ok: …` / `err: …` (never silent clamp).

Requires firmware that includes `note_filter.c`. Old images reply with the
generic unknown-command error (type `h` for the live list).

---

## 7. Scope verification (sine test)

A **sine in → LPF → sine out** at the **same frequency**. You will **not**
see “rounded” square-like shapes. Verify with **amplitude ratio**.

### 7.1 Procedure

```text
n 0
n0
g 1 0
n0 1000
f0 20000                # reference (bypass)
# measure Vpp_ref

f0 300            # engage LPF
# measure Vpp_filt  (same timebase / trigger)
```

Period must stay **~1.00 ms** (1000 Hz). Only **height** changes.

### 7.2 Expected gain (analog prototype, good check at these fc ≪ fs/2)

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

*(Bilinear digital response is extremely close to the analog prototype here
because \(f,f_c \ll 48\,\mathrm{kHz}\). Fixed-point quantization adds a small
extra error; ±10–15% of the ratio is still a pass for bring-up.)*

### 7.3 What “complex” checks look like on a scope

```text
Bypass (fc=20k)          fc=300 Hz, f=1kHz
  ┌─┐   ┌─┐                ┌┐     ┌┐
  │ │   │ │     same T     ││     ││     same period (~1 ms)
  │ │   │ │     ≈1 ms      ││     ││     much smaller Vpp
──┘ └───┘ └──            ──┘└─────┘└──
```

Frequency counter / period cursors: **unchanged**. Peak-to-peak: **down**.

---

## 8. Files / integration

| File                                       | Role                                                        |
| ------------------------------------------ | ----------------------------------------------------------- |
| `Core/Src/filters/butterworth_four_pole.c` | DF4 design + process kernel                                 |
| `Core/Inc/filters/butterworth_four_pole.h` | Kernel API                                                  |
| `Core/Src/filters/note_filter.c`           | Per-voice cutoff/bypass/q + Q31 edges                       |
| `Core/Inc/filters/note_filter.h`           | Public voice API                                            |
| `Core/Src/audio/note_bank.c`               | Calls `NoteFilter_Process` after amp; resets on note-off    |
| `Core/Src/console/channel_console.c`       | `f0`…`f7` / `f` with optional q; init bypass + q=1.0        |
| Top-level `CMakeLists.txt`                 | Registers filter + audio sources under `Core/Src/<domain>/` |

---

## 9. Prompt for ChatGPT (diagrams + compare to bench)

Copy-paste:

```text
Draw diagrams and compute numbers for THIS exact filter so I can compare to an oscilloscope.

## Filter (match firmware)
- 4-pole Butterworth LPF = two cascaded RBJ LPF biquads
- fs = 96000 Hz
- Section Q values in cascade order: Q0 = 0.5411961001461969, Q1 = 1.3065629648763766
- Both sections share the same cutoff fc
- At fc = 20000 Hz the implementation bypasses (gain = 1)
- Input: pure sine at frequency f (DDS); output must still be a sine at f

RBJ LPF per section:
  w0 = 2*pi*fc/fs
  alpha = sin(w0)/(2*Q)
  b0=(1-cos(w0))/2 ; b1=1-cos(w0) ; b2=b0
  a0=1+alpha ; a1=-2*cos(w0) ; a2=1-alpha
  then divide b*,a1,a2 by a0
Cascade H = H_Q0 * H_Q1

Analog check formula (n=4):
  |H| = 1/sqrt(1+(f/fc)^8)

## Primary hardware test case
Commands:
  n0 1000 1.0
  f0 20000   → measure Vpp_ref
  f0 300     → measure Vpp_filt
Also show fc in {200, 300, 500, 1000, 2000, 5000} at f=1000.

### Harmonic / noise demo
Use `p 0.5` or `t 0.5` then sweep `f0` for an audible/filterable harmonic
demo. Sine (`s`) still verifies amplitude drop vs bypass (primary test case).

### Pass modes (LP only in v1)

No `pass` console command in this build (always LP). Example:

```text
f0 500                   # LPF corner (keep current q)
f0 500 5                 # same corner, stronger peak near 500 Hz
f 0                      # bypass all 0..7 (same as f 20000)
```
Query: `f0` → `ok: f0 …. Hz q …` (bypass annotated when 20000).

## What to produce
1) Block diagram: sine → SOS(Q=0.541) → SOS(Q=1.307) → out
2) Bode sketch (mag dB vs log f) for fc=300 and mark f=1000
3) Time-domain sketch: two aligned scope traces
   - Trace A: bypass (full sine, T=1 ms)
   - Trace B: fc=300 (same T, amplitude ≈ G * A)
   Label period and expected Vpp ratio
4) Table: fc, |H| linear, dB, Vpp_filt/Vpp_ref for f=1000
5) At f=fc=1000, show it is −3 dB (Butterworth definition)
6) List what must NOT change on the scope (frequency/period/shape class)
7) If my measured ratio disagrees >15%, likely causes: old firmware without cutoff, probing analog VCF instead of bypass path, midi_host/USB fighting note bank, gain/scale too low for trigger

Use ASCII art or mermaid for every diagram. Show the (f/fc)^8 steps for fc=300, f=1000 explicitly.
```

---

## 10. Related

- Analog wet path (VCF/SCF): [`apps/channel_card/README.md`](../apps/channel_card/README.md)
- Realtime / Q31 rules: `.cursor/rules/audio-dsp-conventions.mdc`,
  `.cursor/rules/realtime-audio-performance.mdc`
