# Fade an active note for at most 2 ms before starting its replacement.
# The replacement also uses a 2 ms attack.
# stage: 0=idle, 1=attack, 2=hold, 3=release, 4=steal

def on_note_on(key, velocity)
    state stage
    if stage != 0
        stage = 4
        ramp(0, 500)
        return
    end
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
    if stage == 4
        start_note()
        set_amplitude(0)
        stage = 1
        ramp(1, 500)
        return
    end
    if stage == 1
        stage = 2
        hold()
        return
    end
    stage = 0
    note_end()
end
