# Event-driven Channel envelope. State 0: attack=1, hold=2, release=3, crash=4.
def on_note_on()
    if input(INPUT_ACTIVE)
        if state_get(0) == 4 return end
        state_set(0, 4)
        var amplitude = input(INPUT_AMPLITUDE)
        if amplitude <= 0
            start_note()
            set_amplitude(0)
            state_set(0, 1)
            ramp(1, 20)
            return
        end
        # This program owns a fixed 3 ms crash fade.
        ramp(0, amplitude / 0.003)
        return
    end
    start_note()
    set_amplitude(0)
    state_set(0, 1)
    ramp(1, 20)
end

def on_note_off()
    if input(INPUT_HAS_PENDING)
        note_end()
        return
    end
    state_set(0, 3)
    ramp(0, 4)
end

def on_ramp_end()
    var stage = state_get(0)
    if stage == 4
        start_note()
        set_amplitude(0)
        state_set(0, 1)
        ramp(1, 20)
        return
    end
    if stage == 3
        note_end()
        return
    end
    state_set(0, 2)
    hold()
end
