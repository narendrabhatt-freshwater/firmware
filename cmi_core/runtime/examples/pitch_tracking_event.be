def on_note_on(key, velocity)
    state pitch_ratio
    var color = (key - 36) / 48.0
    if color < 0 color = 0 end
    if color > 1 color = 1 end
    led(color, 1 - color, 0, 0.8)
    pitch_ratio = pow(2, (key - 60) / 12.0)
    start_note()
    set_amplitude(0)
    ramp(1, 20 * pitch_ratio)
end

def on_note_off()
    ramp(0, 2 / pitch_ratio)
end

def on_ramp_end()
    hold()
end
