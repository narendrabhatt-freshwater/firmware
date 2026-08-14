# Card data flow

Firmware signal paths for the Channel and Effect cards, including
USB/RS485, DMA, and the analog blocks the MCU only steers. Analog
switch polarity and the Channel wet/dry photo are in
[`channel_card_audio_flow.jpg`](channel_card_audio_flow.jpg) and the
Channel card README.

If this document and the firmware disagree, trust the firmware.

## 1. Host interfaces (both cards)

One RS485 multi-drop bus (`c:` / `e:` / `*:`). USB is per-card: Channel
is a Full-Speed **UAC2 speaker** (10ch int16 @ 48 kHz) plus CDC; Effect
is a Full-Speed **UAC2 microphone** (mono int32 @ 96 kHz) plus CDC.

```mermaid
flowchart TB
  subgraph host [Host]
    RS485[RS485 460800 8N1]
    CDC[USB CDC ACM]
    UacOut[UAC2 OUT 10ch int16 48 kHz]
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
  CDC -->|"al 16384 Q31"| ChAtk
  CDC --> ChCon
  CDC --> FxCon
  UacOut -->|"ch0 tag ch1..9 body"| ChRing
  ChAtk --> ChMix
  ChRing --> ChMix
  ChCon --> ChMix
  ChMix --> ChI2S --> ChDac
  FxAdc --> FxSai --> FxFifo --> UacIn
  FxCon -->|"u 1..8"| FxSai
```

## 2. Channel — SAMPLE voice (one of n0..n7)

Attack and body are storage. The 4096-sample head always plays to the end.
Body is a FIFO from the UAC ring, consumed with Q16.16 interpolation.
Live `nX` slews `phase_inc` only. A new UAC session (`SOF` + session 0–6)
starts a new body FIFO; a repeated frame with the same session does not.
The host paces USB frames by `phase_inc` (credit per output sample).
Missing body holds the last sample until USB catches up. The producer
never overwrites unread FIFO samples.

```mermaid
flowchart TB
  subgraph store [Storage]
    File["48 kHz stream"]
    Atk["Attack AXI: file 0..4095 Q31"]
    Body["Host body: file 4064..end int16"]
    File --> Atk
    File --> Body
  end

  subgraph usb [USB FS]
    Tag["UAC frame: ch0 = 0x7F00 | session | SOF | voice; 0x7FFF idle"]
    Slots["8 slots x 256 int16 DTCM"]
    Body --> Tag --> Slots
  end

  subgraph play [Playhead — I2S1 DMA ISR]
    Ph["uint64 Q16.16 phase"]
    Inc["phase_inc = note_Hz / root_Hz; slew toward target"]
    Join{"phase vs 4064 / 4096"}
    AtkOnly["attack lerp"]
    Xfade["overlap mix K=32"]
    BodyOnly["body lerp from ring rd"]
    Lpf["note_filter DF4"]
    Env["note_envelope"]
    Ph --> Join
    Inc --> Ph
    Atk --> AtkOnly
    Atk --> Xfade
    Slots --> Xfade
    Slots --> BodyOnly
    Join -->|lt 224| AtkOnly
    Join -->|224..256| Xfade
    Join -->|ge 256| BodyOnly
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

`vq` reports `ok:vq <mask_hex> <best> s0..s7` (`si` = free slots 0..8).
Need-score is `filled / max(phase_inc, target_inc)`. The host mux does
not use `vq` while a note is sounding — it paces from `phase_inc`.
UAC and `nX` start together. The 4096-sample attack covers the join;
the host does not wait for FIFO prefill. `StreamRing_Prime` arms consume
and does not clear the ring. New voices get USB first until 256 samples
are queued.

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
    USB1[OTG_FS]
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

Channel `vq` replies are sent from the main loop (same loop as
`USB_App_Task`). Do not poll `vq` while a note is sounding — UART TX
starves TinyUSB SOF and ISO OUT packets drop (cracked sustain). Host
mux uses the 1 ms UAC estimate; `vq` is for post-note-off idle only.
