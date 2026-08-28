import math

configure_outputs(3)
rate = define_control("rate", 0.0, 20.0, 1.0, 10)
depth = define_control("depth", 0.0, 1.0, 0.75, 10)
offset = define_control("offset", -1.0, 1.0, 0.0, 10)
shape = define_control("shape", 0.0, 1.0, 0.0, 0)

def tick()
    var phase = tick_index() * 0.001 * control_get(rate)
    phase = phase - int(phase)
    var sine = math.sin(phase * 6.283185307)
    var triangle = phase < 0.5 ? phase * 4.0 - 1.0 : 3.0 - phase * 4.0
    var blend = control_get(shape)
    var value = (sine * (1.0 - blend) + triangle * blend) * control_get(depth) + control_get(offset)
    var voice = 0
    while voice < 8
        output_set(voice, 0, value)
        output_set(voice, 1, sine * control_get(depth))
        output_set(voice, 2, triangle * control_get(depth))
        voice += 1
    end
end
