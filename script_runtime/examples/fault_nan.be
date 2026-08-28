import math
configure_outputs(3)

def tick()
    var value = math.sqrt(-1.0)
    var note = 0
    while note < 8
        output_set(note, 0, value)
        output_set(note, 1, 0.0)
        output_set(note, 2, 0.0)
        note += 1
    end
end
