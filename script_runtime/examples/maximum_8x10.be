configure_outputs(10)
rate = define_control("rate", 0.0, 20.0, 2.0, 0)
depth = define_control("depth", 0.0, 1.0, 1.0, 0)

def tick()
    var phase = (tick_index() % 1000) * 0.001 * control_get(rate)
    phase = phase - int(phase)
    var triangle = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0
    var note = 0
    while note < 8
        var output = 0
        while output < 10
            output_set(note, output, triangle * control_get(depth) * (output + 1) * 0.1)
            output += 1
        end
        note += 1
    end
end
