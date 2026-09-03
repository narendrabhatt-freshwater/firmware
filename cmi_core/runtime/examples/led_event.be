def on_note_on(key, velocity)
    var color = (key - 36) / 48.0
    if color < 0 color = 0 end
    if color > 1 color = 1 end
    led(color, 1 - color, 0, 0.8)
end

def on_note_off()
end

def on_ramp_end()
end
