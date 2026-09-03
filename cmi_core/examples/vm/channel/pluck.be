# A 2 ms attack followed by a 500 ms automatic decay.
# stage: 1=attack, 2=decay, 3=release

def on_note_on(key, velocity)
    state stage
    start_note()
    set_amplitude(0)
    stage = 1
    ramp(1, 500)
end

def on_note_off(has_pending)
    if has_pending discard_pending() end
    stage = 3
    ramp(0, 20)
end

def on_ramp_end()
    if stage == 1
        stage = 2
        ramp(0, 2)
        return
    end
    stage = 0
    note_end()
end
