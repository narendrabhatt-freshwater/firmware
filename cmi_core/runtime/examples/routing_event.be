def on_note_on(key, velocity)
    var modulator = osc(0, 7000)
    var carrier = osc(1, pitch_for_key(key))
    modulate(modulator, carrier, FREQUENCY, 250)
    route(carrier, OUTPUT, 0.5)
    start_note()
end

def on_note_off()
    note_end()
end

def on_ramp_end()
end
