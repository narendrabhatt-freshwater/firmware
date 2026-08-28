configure_outputs(3)
rate = define_control("rate", 0.0, 10.0, 1.0, 0)
depth = define_control("depth", 0.0, 1.0, 0.8, 0)
offset = define_control("offset", -1.0, 1.0, 0.0, 0)

def tick()
    var phase = (tick_index() % 1000) * 0.001 * control_get(rate)
    phase = phase - int(phase)
    var triangle = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0
    var value = triangle * control_get(depth) + control_get(offset)
    var note = 0
    while note < 8
        output_set(note, 0, value)
        output_set(note, 1, -value)
        output_set(note, 2, value * 0.5)
        note += 1
    end
end
