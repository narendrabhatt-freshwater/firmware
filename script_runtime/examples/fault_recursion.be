configure_outputs(3)

def recurse(value)
    return recurse(value + 1)
end

def tick()
    recurse(0)
end
