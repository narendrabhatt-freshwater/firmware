# Event-driven attack and release envelope with retrigger protection.
# stage: 1=attack, 2=holding, 3=release, 4=crash release

def on_note_on(key)
    state stage
    var color = (keymap_get(key) - 36) / 48.0
    if color < 0 color = 0 end
    if color > 1 color = 1 end
    led(color, 1 - color, 0, 0.8)
    var active = input(INPUT_ACTIVE)
    var amplitude = input(INPUT_AMPLITUDE)
    if active
        if stage == 4 return end                    # A crash fade is already running.
        stage = 4                                  # Fade out the active note first.
        if amplitude <= 0
            start_note()       # The old note is already silent; switch now.
            set_amplitude(0)  # Restart the envelope from silence.
            stage = 1         # Enter attack.
            ramp(1, 20)       # 0.0 -> 1.0 at slope 20 (50 ms).
            return
        end
        ramp(0, amplitude / 0.003)  # Give the old note a fixed 3 ms fade.
        return
    end

    start_note()       # No active note: begin immediately.
    set_amplitude(0)  # Restart the envelope from silence.
    stage = 1         # Enter attack.
    ramp(1, 20)       # 0.0 -> 1.0 at slope 20 (50 ms).
end

def on_note_off(has_pending)
    if has_pending
        note_end()  # A queued note owns the next transition.
        return
    end

    stage = 3   # Enter release.
    ramp(0, 4)  # Current amplitude -> 0.0 at slope 4.
end

def on_ramp_end()
    if stage == 4
        start_note()       # The crash fade finished; activate the new note.
        set_amplitude(0)  # Restart its envelope from silence.
        stage = 1         # Enter attack.
        ramp(1, 20)       # 0.0 -> 1.0 at slope 20 (50 ms).
        return
    end

    if stage == 3
        note_end()  # Release finished; retire the voice.
        return
    end

    stage = 2  # Attack finished; hold at full amplitude.
    hold()     # Stay here until note-off.
end
