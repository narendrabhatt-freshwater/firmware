def on_note_on(key)
    state pitch_ratio
    pitch_ratio = pow(2, (keymap_get(key) - 60) / 12.0)
    start_note()
    set_amplitude(0)
    ramp(1, 20 * pitch_ratio)
end

def on_note_off(has_pending)
    ramp(0, 2 / pitch_ratio)
end

def on_ramp_end()
    hold()
end
