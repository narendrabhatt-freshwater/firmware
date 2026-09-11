# Ignore note-off deliberately; only a hard stop can reset this state.
def on_note_on(key, velocity)
    state stage
    if stage != 0
        return
    end
    stage = 1
    start_note()
    set_amplitude(1)
    led(1, 0.5, 0.25, 1)
end

def on_note_off()
    return
end

def on_ramp_end()
    return
end
