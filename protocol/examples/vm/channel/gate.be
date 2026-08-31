# Immediate gate with a short click-free release. State 0: release=1, crash=2.
def on_note_on()
    if input(INPUT_ACTIVE)
        state_set(0, 2)
        var amplitude = input(INPUT_AMPLITUDE)
        if amplitude <= 0
            start_note()
            set_amplitude(1)
            hold()
            state_set(0, 0)
            return
        end
        # This program owns a fixed 3 ms crash fade.
        ramp(0, amplitude / 0.003)
        return
    end
    start_note()
    set_amplitude(1)
    hold()
    state_set(0, 0)
end

def on_note_off()
    if input(INPUT_HAS_PENDING)
        note_end()
        return
    end
    state_set(0, 1)
    ramp(0, 20)
end

def on_ramp_end()
    if state_get(0) == 2
        start_note()
        set_amplitude(1)
        hold()
        state_set(0, 0)
        return
    end
    note_end()
end
