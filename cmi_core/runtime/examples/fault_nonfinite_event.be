def on_note_on(key, velocity)
    set_amplitude(input(INPUT_FREQUENCY))
end
def on_note_off(has_pending) end
def on_ramp_end() end
