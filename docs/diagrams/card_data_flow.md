# Card data flow

Firmware signal paths for the Channel and Effect cards, including
USB/RS485, DMA, and the analog blocks the MCU only steers. Analog
switch polarity and the Channel wet/dry photo are in
[`channel_card_audio_flow.jpg`](channel_card_audio_flow.jpg) and the
Channel card README.

If this document and the firmware disagree, trust the firmware.

## 1. Host interfaces (both cards)

One RS485 multi-drop bus (`c:` / `e:` / `*:`). USB is per-card: Channel
uses one binary Full-Speed CDC connection for BODY and uploads. RS485 `vq`
provides exact ring credit and processed-BODY acknowledgements, normally every
5 ms and as often as 1 ms for high source demand. Effect retains its Full-Speed
UAC2 microphone (mono int32 at 96 kHz) and separate CDC text console.

```mermaid
flowchart TB
  subgraph host [Host]
    RS485[RS485 921600 8N1]
    CDC[Effect USB CDC console]
    BodyOut[Channel binary CDC BODY and uploads]
    UacIn[UAC2 IN mono int32 96 kHz]
  end

  subgraph ch [Channel STM32H725]
    ChCon[channel_console]
    ChAtk[AXI attack heads and script uploads]
    ChRing[Contiguous DTCM BODY rings]
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

  RS485 -->|"c: nX f vq"| ChCon
  RS485 -->|"e: u echo"| FxCon
  BodyOut -->|"typed upload blocks"| ChAtk
  CDC --> FxCon
  BodyOut -->|"voice session SOF int8"| ChRing
  ChAtk --> ChMix
  ChRing --> ChMix
  ChCon --> ChMix
  ChMix --> ChI2S --> ChDac
  FxAdc --> FxSai --> FxFifo --> UacIn
  FxCon -->|"u 1..8"| FxSai
```

## 2. Channel — SAMPLE voice (one of n0..n7)

Attack and BODY are storage. The attack plays to its committed length (up to
512 samples), joining the signed-int8 BODY ring through the existing 32-sample
overlap. A note-on over RS485 arms a session before acknowledging it. The host
then sends BODY for that session over the shared binary CDC stream. The card
waits for 998 samples before dispatching the script's note-on at an audio
boundary. Upload blocks can be interleaved with BODY; no audio-class carrier
or continuous idle padding is involved.

```mermaid
flowchart TB
  subgraph store [Storage]
    File["48 kHz stream"]
    Atk["Attack AXI: 256 heads x 512 int8"]
    Osc["Auto-allocated looped oscillators<br/>logical 0..7 = attack IDs 248..255"]
    Body["Host body: file (len-32)..end int8"]
    File --> Atk
    File --> Body
    Atk --> Osc
  end

  subgraph usb [USB FS]
    Tag["BODY: 4-byte metadata + signed int8 samples"]
    Slots["4080 int8 per voice · current/pending spans · contiguous DTCM"]
    Body --> Tag --> Slots
  end

  subgraph play [Playhead — I2S1 DMA ISR]
    Ph["uint64 Q16.16 phase"]
    Inc["phase_inc = note_Hz / root_Hz; slew toward target"]
    Join{"phase vs len-32 / len"}
    AtkOnly["attack lerp"]
    Xfade["overlap: attack out, body consume"]
    BodyOnly["body lerp from ring rd"]
    SourceMix["average sample + enabled oscillators"]
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
    AtkOnly --> SourceMix
    Xfade --> SourceMix
    BodyOnly --> SourceMix
    Osc --> SourceMix
    SourceMix --> Lpf
    Lpf --> Env
  end

  subgraph mix [CH1 mix]
    Sum["sum 8 voices saturate Q31"]
    Env --> Sum
  end
```

RS485 `vq` returns a 61-byte frame containing masks, physical ring capacity,
sessions, exact free space, source demand, playback duration and the last
processed BODY sequence. Host credits subtract every unacknowledged sample,
including old sessions sharing a ring. Startup and endangered voices take
priority; within a group, fewer in-flight samples and deadlines determine order.

BODY blocks contain 1..1024 samples for one voice. Complete blocks publish through
a ring reservation; a pending-to-playing promotion during the copy preserves
valid data, while a superseded session cannot publish. Release, pitch, envelopes,
filtering and oscillator routing remain owned by the existing note/VM engine.

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
    Tud1[USB_App_Task: parse BODY and uploads]
    Con1[Console_Poll RS485]
    Tud1 --- Con1
  end

  subgraph ch_isr [Channel ISRs]
    I2S1[I2S1 DMA: note mix + CH2]
    I2S2[I2S2 DMA: CH3/CH4]
    U5[UART5 RX]
    USB1[HAL PCD: OUT queue and immediate rearm]
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

RS485 `vq` provides the refill permission and last processed BODY sequence.
USB carries framed BODY and uploads. The Channel receive queue has 8192 bytes;
when full, the endpoint stays unarmed and NAKs until space becomes available.
The I2S/DAC clock runs independently of USB traffic. Effect retains TinyUSB.
