def on_note_on(key, velocity)
    var frequency = pitch_for_key(key)
    osc(0, frequency)
    osc(1, frequency * 2)
    osc(2, frequency * 3)
    osc(3, frequency * 4)
    osc(4, frequency * 5)
    osc(5, frequency * 6)
    osc(6, frequency * 7)
    osc(7, frequency * 8)
    osc(0, frequency * 9)
    osc(1, frequency * 10)
    start_note()
end

def on_note_off()
    note_end()
end

def on_ramp_end()
end
