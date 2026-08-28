configure_outputs(3)

def tick()
    var forbidden = "tick-" + tick_index()
    output_set(0, 0, forbidden)
end
