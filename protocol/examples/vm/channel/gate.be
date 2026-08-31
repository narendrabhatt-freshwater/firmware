# Immediate gate with a short, click-free release.
# stage: 0=holding, 1=release, 2=crash release

def on_note_on(key)
    state stage
    var active = input(INPUT_ACTIVE)
    var amplitude = input(INPUT_AMPLITUDE)
    if active
        stage = 2                                  # Fade out the active note first.
        if amplitude <= 0
            start_note()       # The old note is already silent; switch now.
            set_amplitude(1)  # Open the gate fully.
            hold()            # Keep this level until note-off.
            stage = 0         # The new note is holding.
            return
        end
        ramp(0, amplitude / 0.003)  # Give the old note a fixed 3 ms fade.
        return
    end

    start_note()       # No active note: begin immediately.
    set_amplitude(1)  # Open the gate fully.
    hold()            # Keep this level until note-off.
    stage = 0         # The note is holding.
end

def on_note_off(has_pending)
    if has_pending
        note_end()  # A queued note owns the next transition.
        return
    end

    stage = 1    # Enter the normal release.
    ramp(0, 20)  # Current amplitude -> 0.0 at slope 20.
end

def on_ramp_end()
    if stage == 2
        start_note()       # The crash fade finished; activate the new note.
        set_amplitude(1)  # Open its gate fully.
        hold()            # Keep this level until note-off.
        stage = 0         # The new note is holding.
        return
    end

    note_end()  # The normal release finished; retire the voice.
end
