# A 2 ms attack, held level, and 50 ms release.
# stage: 1=attack, 2=hold, 3=release

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
        hold()
        return
    end
    stage = 0
    note_end()
end
