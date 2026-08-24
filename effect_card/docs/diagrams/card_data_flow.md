# Card data flow

Firmware signal paths for the Channel and Effect cards, including
USB/RS485, DMA, and the analog blocks the MCU only steers. Analog
switch polarity and the Channel wet/dry photo are in
[`channel_card_audio_flow.jpg`](channel_card_audio_flow.jpg) and the
Channel card README.

If this document and the firmware disagree, trust the firmware.

## 1. Host interfaces (both cards)

One RS485 multi-drop bus (`c:` / `e:` / `*:`). USB is per-card: Channel
is Full-Speed **vendor bulk BODY/status** (ITF0, packed OUT quanta up to
9472 bytes per catch-up transfer, plus exact status IN every 1 ms) plus CDC;
Effect is a Full-Speed **UAC2 microphone** (mono int32 @ 96 kHz) plus CDC.

```mermaid
flowchart TB
  subgraph host [Host]
    RS485[RS485 921600 8N1]
    CDC[USB CDC ACM]
    BodyOut[vendor bulk BODY]
    UacIn[UAC2 IN mono int32 96 kHz]
  end

  subgraph ch [Channel STM32H725]
    ChCon[channel_console]
    ChAtk[AXI attack heads al]
    ChRing[DTCM body slots]
    ChMix[note_bank mix]
    ChI2S[I2S1/I2S2 DMA AXI]
    ChDac[CS4304]
  end

  subgraph fx [Effect STM32H743]
    FxCon[effect_console]
    FxSai[SAI1 TDM DMA AXI]
    FxAdc[TLV320ADC6140 x2]
    FxFifo[TinyUSB ISO FIFO]
  end

  RS485 -->|"c: nX en f vq"| ChCon
  RS485 -->|"e: u echo"| FxCon
  CDC -->|"al nbytes int16"| ChAtk
  CDC --> ChCon
  CDC --> FxCon
  BodyOut -->|"voice session SOF int16"| ChRing
  ChAtk --> ChMix
  ChRing --> ChMix
  ChCon --> ChMix
  ChMix --> ChI2S --> ChDac
  FxAdc --> FxSai --> FxFifo --> UacIn
  FxCon -->|"u 1..8"| FxSai
```

## 2. Channel — SAMPLE voice (one of n0..n7)

Attack and body are storage. The head plays to its committed length
(≤ 512). Body is a FIFO from vendor bulk BODY packs, consumed with
Q16.16 interpolation. `nX > 0` is always a note-on. A new BODY session
(`SOF` + session 0–6) starts a new body FIFO; a repeated burst with
the same session does not.
The host packs every wanting voice into one bulk transfer (fair share
of a 9472-byte catch-up FS OUT budget, weighted by each voice's
source-consumption rate). Every fresh USB vendor-IN `vq` exact free-space
grant permits one bounded packed refill; otherwise the host waits for the
next status. A new session gets one safe SOF prefill before `nX`; later
refills are status-gated. This keeps the jitter rings full at note start.
Free-slot code 0 is a hard stop. A full vendor FIFO NAKs the host.
Missing body holds the last sample until USB catches up. A full ring
drops the whole chunk; the producer never overwrites unread FIFO samples.

```mermaid
flowchart TB
  subgraph store [Storage]
    File["48 kHz stream"]
    Atk["Attack AXI: 256 heads x 512 int16"]
    Body["Host body: file (len-32)..end int16"]
    File --> Atk
    File --> Body
  end

  subgraph usb [USB FS]
    Tag["BODY: hdr + voice/session/SOF + int16"]
    Slots["2 x 2816 int16 DTCM ping-pong"]
    Body --> Tag --> Slots
  end

  subgraph play [Playhead — I2S1 DMA ISR]
    Ph["uint64 Q16.16 phase"]
    Inc["phase_inc = note_Hz / root_Hz; slew toward target"]
    Join{"phase vs len-32 / len"}
    AtkOnly["attack lerp"]
    Xfade["overlap: attack out, body consume"]
    BodyOnly["body lerp from ring rd"]
    Lpf["note_filter DF4"]
    Env["note_envelope"]
    Ph --> Join
    Inc --> Ph
    Atk --> AtkOnly
    Atk --> Xfade
    Slots --> Xfade
    Slots --> BodyOnly
    Join -->|lt len-32| AtkOnly
    Join -->|overlap| Xfade
    Join -->|ge len| BodyOnly
    AtkOnly --> Lpf
    Xfade --> Lpf
    BodyOnly --> Lpf
    Lpf --> Env
  end

  subgraph mix [CH1 mix]
    Sum["sum 8 voices saturate Q31"]
    Env --> Sum
  end
```

USB vendor IN pushes a 28-byte framed `vq` status containing active mask,
hungriest voice, eight exact uint16 free-sample counts, and a sequence.
RS485 `vq` returns the same state in its compact 12-byte diagnostic frame.
Need-score is remaining play time: `filled / max(phase_inc, target_inc)`.
The host shares one PACK by the wanting voices' source-consumption rates.
At most one packed refill follows each fresh `vq`; each voice is bounded to
512 samples and its safe free-space credit. A dropped USB write is AbortBurst.
`nX` starts immediately. The attack plays to its committed length;
body consume starts at `len − 32` with the same source fraction. The
first requested-gate `vq` permits the SOF BODY burst; further bursts require
fresh status too. `StreamRing_Prime` arms consume
and does not clear the ring. The host fills the body FIFO, then holds
the file cursor until the playhead reaches the join — dropped USB must
not skip ahead in the wav.

Voices with no loaded attack head play body from the FIFO immediately.
`en` / `f` / `fk` still apply.

## 3. Channel — I2S, DAC, analog

I2S1 half-buffer is 1 ms (48 frames @ 48 kHz) in AXI `.dma_buffer`
(DMA1 cannot read DTCM). Fill runs in the I2S1 DMA half/full ISR.
I2S2 (CH3/CH4) is SPI slave TX: TIM7 clears UDR; `IOSWP` because the
board wires MOSI to SDIN2.

```mermaid
flowchart LR
  subgraph isr [I2S1 DMA ISR 1 ms]
    NB[NoteBank_NextSample]
    DC2[CH2 DC VCA CV]
    Mix["I2S1 L=CH1 audio  R=CH2 CV"]
    NB --> Mix
    DC2 --> Mix
  end

  subgraph i2s2 [I2S2 DMA]
    DC3[CH3 DC VCF cutoff]
    DC4[CH4 DC VCF resonance]
    Mix2["I2S2 L=CH3  R=CH4"]
    DC3 --> Mix2
    DC4 --> Mix2
  end

  subgraph dac [CS4304]
    D1[CH1 audio]
    D2[CH2 VCA CV]
    D3[CH3 cutoff CV]
    D4[CH4 resonance CV]
  end

  Mix --> D1
  Mix --> D2
  Mix2 --> D3
  Mix2 --> D4

  subgraph analog [Analog — GPIO switches]
    Dry[bypass dry to out]
    Scf[SCF]
    Vcf[VCF lp/bp/hp taps]
    Vca[VCA]
    Out[out]
    D1 --> Dry --> Out
    D1 --> Scf --> Vca
    D1 --> Vcf --> Vca
    Vca --> Out
    D2 --> Vca
    D3 --> Vcf
    D4 --> Vcf
  end
```

Switch names and polarity: Channel card README (`switches[]` in
`channel_console.c`). SCF clock is `filter_ctl` (TIM3_CH1); cutoff is
clock ÷ 100.

## 4. Effect — capture to USB

Both ADCs share BCLK/FSYNC from SAI1_A. USB Full-Speed cannot carry
eight 96 kHz/32-bit channels; `u 1..8` selects one slot into the ISO IN
FIFO. SAI `FSOffset = SAI_FS_BEFOREFIRSTBIT` — the TLV320 starts slot 0
one BCLK after FSYNC; without that the sign bit is the previous slot LSB.

```mermaid
flowchart TB
  subgraph analog_in [Analog in]
    In1["u1..u4"]
    In2["u5..u8"]
  end

  subgraph adc [TLV320ADC6140]
    A1["ADC1 I2C 0x4C"]
    A2["ADC2 I2C 0x4D"]
    In1 --> A1
    In2 --> A2
  end

  subgraph sai [SAI1 TDM 96 kHz]
    B["SAI1_B slave RX — slots 0..3"]
    A["SAI1_A master RX — slots 0..3"]
    A1 --> B
    A2 --> A
  end

  subgraph dma [Circular DMA AXI 1 ms halves]
    RA["sai_rx_b 96 frames x 4"]
    RB["sai_rx_a 96 frames x 4"]
    B --> RA
    A --> RB
  end

  subgraph pick [Half/full callback]
    Sel["u 1..8 → block + slot"]
    Mono["96 mono int32"]
    RA --> Sel
    RB --> Sel
    Sel --> Mono
  end

  subgraph usb [TinyUSB]
    Fifo[ISO IN FIFO]
    Mic[UAC2 mic 32-bit 96 kHz]
    Mono --> Fifo --> Mic
  end
```

48 V phantom enable and power-good are console/GPIO only; they are not
in the sample path.

## 5. Main-loop vs ISR (both cards)

```mermaid
flowchart LR
  subgraph ch_loop [Channel main loop]
    Tud1[tud_task / USB_App_Task]
    Con1[Console_Poll RS485]
    Tud1 --- Con1
  end

  subgraph ch_isr [Channel ISRs]
    I2S1[I2S1 DMA: note mix + CH2]
    I2S2[I2S2 DMA: CH3/CH4]
    U5[UART5 RX]
    USB1[OTG_HS]
  end

  subgraph fx_loop [Effect main loop]
    Tud2[tud_task]
    Con2[Console_Poll RS485]
    Tud2 --- Con2
  end

  subgraph fx_isr [Effect ISRs]
    SaiDma[SAI DMA half/full: de-interleave + FIFO]
    USB2[OTG_FS]
  end
```

Channel USB `vq` status is pushed from `USB_App_Task` and endpoint
backpressure limits it to host service rate. It is the refill permission.
The slower RS485 `vq` poll remains for lifecycle monitoring and diagnostics;
it does not authorize BODY. A full vendor FIFO NAKs the host.
