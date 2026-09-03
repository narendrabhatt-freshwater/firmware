# Complete Channel voice program.
#
# Active behaviour:
# - standard MIDI pitch (A4 = 440 Hz)
# - MIDI velocity controls the held amplitude
# - fixed 50 ms attack
# - at most 2 ms fade before a pending note steals this voice
# - at most 50 ms release
#
# stage: 0=idle, 1=attack, 2=hold, 3=release, 4=voice steal

def on_note_on(key, velocity)
    state stage
    state level

    # Amplitude choices (leave only one assignment active).
    level = velocity / 127.0                 # linear MIDI velocity
    # level = 1                              # ignore MIDI velocity
    # level = pow(velocity / 127.0, 2)       # quieter, squared response

    if stage != 0
        # A newer pending note replaces the previous pending note while the
        # current voice-steal fade continues. `level` already has its velocity.
        if stage == 4
            return
        end

        stage = 4
        ramp(0, 500)                          # full scale -> 0 in 2 ms
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
    ramp(level, level / 0.05)                 # silence -> level in 50 ms

    # Optional 100 ms velocity-brightness flash.
    # led(0.1, 0.4, 1.0, level)
end

def on_note_off()
    stage = 3
    ramp(0, 20)                               # full scale -> 0 in 50 ms
end

def on_ramp_end()
    if stage == 4
        start_note()
        set_amplitude(0)
        stage = 1
        ramp(level, level / 0.05)             # replacement attack: 50 ms
        return
    end
    if stage == 1
        stage = 2
        return
    end
    stage = 0
    note_end()
end

# Other envelope recipes (examples only; the program above stays active):
#
# Immediate velocity gate instead of a 50 ms attack:
#     set_amplitude(level)
#     stage = 2
#
# Fixed 2 ms attack instead of a 50 ms attack:
#     set_amplitude(0)
#     stage = 1
#     ramp(level, level / 0.002)
#
# A pluck can start a decay from the stage-1 branch in on_ramp_end():
#     stage = 3
#     ramp(0, level / 0.5)                    # decay to zero in 500 ms
# (Use a separate stage value if note-off needs different cleanup behaviour.)
#
# Other actions:
# discard_pending()                           # reject a pending replacement
# note_end()                                  # retire the current note
