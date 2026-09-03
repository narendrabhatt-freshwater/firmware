def on_note_on(key)
    var color = (keymap_get(key) - 36) / 48.0
    if color < 0 color = 0 end
    if color > 1 color = 1 end
    led(color, 1 - color, 0, 0.8)
end

def on_note_off(has_pending)
end

def on_ramp_end()
end
