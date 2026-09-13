# stage: 0=idle, 1=attack, 2=hold, 3=release, 4=voice steal

def on_note_on(key, velocity)
    state stage
    state hold_amplitude
    state attack_slope
    state decay
    state led_level
    var attack = 20
    var level = 1
    decay = 200
    led_level = velocity / 127.0

    hold_amplitude = level
    attack_slope = attack * (velocity / 127.0)

    if stage != 0
        if stage == 4 return end
        stage = 4
        ramp(0, 200)  # Full scale to silence in 4 ms.
        return
    end

    start_note()
    set_amplitude(0)
    if attack_slope == 0
        set_amplitude(hold_amplitude)
        stage = 2
    else
        stage = 1
        ramp(hold_amplitude, attack_slope)
    end
    led(0, 1, 0, led_level)
end

def on_note_off()
    stage = 3
    ramp(0, decay)
end

def on_ramp_end()
    if stage == 4
        start_note()
        set_amplitude(0)
        if attack_slope == 0
            set_amplitude(hold_amplitude)
            stage = 2
        else
            stage = 1
            ramp(hold_amplitude, attack_slope)
        end
        led(0, 1, 0, led_level)
    elif stage == 3
        stage = 0
        led(0, 0, 0, 0)
        note_end()
    elif stage == 1
        stage = 2
    end
end
