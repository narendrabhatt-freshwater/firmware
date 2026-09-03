def on_note_on(key, velocity)
    var frequency = pitch_for_key(key)
    state first_handle = osc(0, frequency)
    osc(0, frequency * 2)
    start_note()
end

def on_note_off()
    note_end()
end

def on_ramp_end()
end
