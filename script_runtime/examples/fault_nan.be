import math
configure_outputs(3)

def tick()
    var value = math.sqrt(-1.0)
    var voice = 0
    while voice < 8
        output_set(voice, 0, value)
        output_set(voice, 1, 0.0)
        output_set(voice, 2, 0.0)
        voice += 1
    end
end
