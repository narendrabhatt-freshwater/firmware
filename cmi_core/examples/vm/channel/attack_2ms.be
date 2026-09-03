# Complete playable Channel voice template.
# Active behaviour:
# - standard MIDI pitch (A4 = 440 Hz)
# - normal chromatic keyboard pitch
# - 2 ms attack, held level, and 50 ms release
#
# transpose notes, remap keys, use velocity, or flash the card LED.
# stage: 1=attack, 2=hold, 3=release

def on_note_on(key, velocity)
    state stage

    # Normal MIDI pitch. This lookup is only a convenient reference.
    # start_note(pitch_for_key(key))

    start_note()                                # default MIDI pitch
    # start_note(440.0)                           # fixed A4 on every key
    # start_note(pitch_for_key(key) * 2.0)        # one octave up
    # start_note(pitch_for_key(key) * 0.5)        # one octave down
    # start_note(pitch_for_key(key) * pow(2, 7 / 12.0)) # seven semitones up

    # set_amplitude(0)

    set_amplitude(velocity / 127.0)

    led(0.1, 0.4, 1.0, velocity / 127.0)

    stage = 1
    ramp(1, 500)
end

def on_note_off()
    stage = 3
    ramp(0, 20)
end

def on_ramp_end()
    if stage == 1
        stage = 2
        hold()
        return
    end
    stage = 0
    note_end()
end
