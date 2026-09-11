def on_note_on(key, velocity)
    state releasing
    state slope
    releasing = 0
    slope = 0
    if key == 60
        slope = -48000.0
    elif key == 61
        slope = 3.0e38
    elif key == 62
        slope = -3.0e38
    end
    start_note()
    set_amplitude(200)
    set_amplitude(-200)
    ramp(200, slope)
end

def on_note_off()
    releasing = 1
    ramp(-200, slope)
end

def on_ramp_end()
    if releasing note_end() end
end
