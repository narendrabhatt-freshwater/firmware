# Velocity-controlled gate with a 20 ms click-free release.
# MIDI velocity is an integer from 1 through 127.
# stage: 1=hold, 2=release

def on_note_on(key, velocity)
    state stage
    start_note()
    set_amplitude(velocity / 127.0)
    stage = 1
end

def on_note_off()
    stage = 2
    ramp(0, 50)
end

def on_ramp_end()
    stage = 0
    note_end()
end
