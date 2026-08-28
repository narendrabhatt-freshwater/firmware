configure_outputs(3)
attack = define_control("attack", 1.0, 1000.0, 50.0, 0)
release = define_control("release", 1.0, 2000.0, 250.0, 0)
level0 = 0.0
level1 = 0.0
level2 = 0.0
level3 = 0.0
level4 = 0.0
level5 = 0.0
level6 = 0.0
level7 = 0.0

def advance(note, level)
    if note_started(note)
        level = 0.0
    end
    if note_is_on(note)
        level += 1.0 / control_get(attack)
        if level > 1.0 level = 1.0 end
    else
        level -= 1.0 / control_get(release)
        if level < 0.0 level = 0.0 end
    end
    return level
end

def emit(note, level)
    output_set(note, 0, level)
    output_set(note, 1, level * level)
    output_set(note, 2, 1.0 - level)
end

def tick()
    level0 = advance(0, level0)
    emit(0, level0)
    level1 = advance(1, level1)
    emit(1, level1)
    level2 = advance(2, level2)
    emit(2, level2)
    level3 = advance(3, level3)
    emit(3, level3)
    level4 = advance(4, level4)
    emit(4, level4)
    level5 = advance(5, level5)
    emit(5, level5)
    level6 = advance(6, level6)
    emit(6, level6)
    level7 = advance(7, level7)
    emit(7, level7)
end
