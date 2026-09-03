# stage: 0=idle, 1=attack, 2=decay, 3=hold, 4=release, 5=voice steal

def on_note_on(key, velocity)
    state stage
    state level
    state sustain
    state attack_slope

    # Amplitude choices (leave only one assignment active).
    level = velocity / 127.0                 # linear MIDI velocity
    # level = 1                              # ignore MIDI velocity
    # level = pow(velocity / 127.0, 2)       # quieter, squared response

    # Higher velocity gives a faster attack:
    # velocity 127 ~= 50 ms, 64 ~= 100 ms, and 32 ~= 200 ms.
    attack_slope = level * level * 20

    # Sustain choices (leave only one assignment active).
    sustain = level * 0.9             # hold at 90% of the peak

    var pitch = pitch_for_key(key)

    # sustain = level                        # no decay; hold at the peak
    # sustain = level * 0.5                  # hold at 50% of the peak

    if stage != 0
        # A newer pending note replaces the previous pending note while the
        # current voice-steal fade continues. `level` already has its velocity.
        if stage == 5
            return
        end

        stage = 5
        ramp(0, 200)                          # full scale -> 0 in 5 ms
        return
    end

    # 8 oscillators, + 50hz each
    # for i : 0..2
    #     osc(0, pitch + 5 * (i + 1))
    # end

    # ROUTING REFERENCE
    # route(source, OUTPUT, output_weight) sends oscillator audio to the output.
    # modulate(source, target, control, amount) changes a target control.
    #
    # Available source:
    #   An oscillator handle returned by osc(). SAMPLE cannot be a source yet.
    #
    # Available targets:
    #   OUTPUT = the audible voice output
    #   SAMPLE = this voice's streamed attack/body sample
    #   another oscillator handle
    #
    # Available modulation controls:
    #   FREQUENCY = source moves target pitch; amount is +/-Hz
    #   AMPLITUDE = source controls target level; amount is gain 0..1
    #
    # All currently valid connections:
    # route(source, OUTPUT, output_weight)
    # modulate(source, SAMPLE,  FREQUENCY, deviation_hz)
    # modulate(source, SAMPLE,  AMPLITUDE, gain)
    # modulate(source, another, FREQUENCY, deviation_hz)
    # modulate(source, another, AMPLITUDE, gain)
    #
    # Simple tremolo: move the sample between silence and 50% level at 5 Hz.
    # var tremolo = osc(0, 5)
    # modulate(tremolo, SAMPLE, AMPLITUDE, 0.5)
    #
    # Simple vibrato: move the sample pitch at 5 Hz by +/-10 Hz.
    var vibrato = osc(0, 5)
    modulate(vibrato, SAMPLE, FREQUENCY, 10)

    var oscillator_fm = osc(0, pitch)

    var vibrato1 = osc(0, 7)
    modulate(vibrato1, oscillator_fm, FREQUENCY, 10)

    #
    # Simple oscillator FM: move a carrier pitch by +/-250 Hz.
    # var modulator = osc(0, 7000)
    # var carrier = osc(1, pitch)
    # modulate(modulator, carrier, FREQUENCY, 250)
    # modulate(modulator, carrier, AMPLITUDE, 0.5)
    # route(carrier, OUTPUT, 1.0)
    #
    # An oscillator stops going directly to OUTPUT after its first explicit
    # modulation. Add route(source, OUTPUT, weight) when it should remain
    # directly audible too.

    start_note()                              # standard MIDI pitch
    # start_note(440.0)                       # fixed A4 on every key
    # start_note(pitch_for_key(key) * 2.0)    # one octave up
    # start_note(pitch_for_key(key) * 0.5)    # one octave down
    # start_note(pitch_for_key(key) * pow(2, 7 / 12.0)) # seven semitones up

    set_amplitude(0)
    stage = 1
    ramp(level, attack_slope)                 # velocity controls attack time

    # Keep RGB on for the note, with brightness controlled by velocity.
    led(0.1, 0.4, 1.0, level)
end

def on_note_off()
    # Manual release
    stage = 4
    ramp(0, 20)                             # full scale -> 0 in 50 ms
end

def on_ramp_end()
    # Switch-style stage dispatcher. The Channel Berry subset uses if/elif
    # rather than a switch/case statement.
    if stage == 5
        start_note()
        set_amplitude(0)
        stage = 1
        ramp(level, attack_slope)             # replacement keeps its velocity attack

        led(0.1, 0.4, 1.0, level)              # show the replacement note
        return
    elif stage == 1
        stage = 2
        ramp(sustain, (level - sustain) / 0.5) # peak -> sustain in 500 ms
        return
    elif stage == 2
        stage = 3                             # hold until note-off
        return
    end
    stage = 0
    led(0, 0, 0, 0)                            # RGB off when the note ends
    note_end()
end                              # retire the current note
