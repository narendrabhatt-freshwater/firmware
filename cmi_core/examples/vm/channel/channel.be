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
    sustain = level * 0.2                    # hold at 20% of the peak
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

    # Pitch choices (leave only one start_note call active).
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
