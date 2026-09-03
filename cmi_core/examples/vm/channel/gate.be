# Immediate note start with a 20 ms click-free release.
# stage: 1=hold, 2=release

def on_note_on(key, velocity)
    state stage
    start_note()
    set_amplitude(1)
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
