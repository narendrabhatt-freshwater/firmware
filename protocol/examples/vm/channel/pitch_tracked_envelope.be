# Simple envelope with independent pitch tracking for each segment.
# Script calculates each pitch-dependent slope with pow(); ramp itself is neutral.
# stage: 0=idle, 1=attack, 2=decay, 3=sustain, 4=release

def on_note_on(key)
    state stage
    state pitch_ratio
    var color = (keymap_get(key) - 36) / 48.0
    if color < 0 color = 0 end
    if color > 1 color = 1 end
    led(color, 1 - color, 0, 0.8)
    pitch_ratio = pow(2, (keymap_get(key) - 60) / 12.0)
    start_note()          # Immediately make the received note active.
    set_amplitude(0)      # Restart its envelope from silence.
    stage = 1             # Enter attack.
    ramp(1, 20 * pitch_ratio) # Full tracking: each octave up halves attack time.
end

def on_note_off(has_pending)
    stage = 4             # Enter release.
    ramp(0, 2 / pitch_ratio)  # Inverse tracking for longer high-note tails.
end

def on_ramp_end()
    if stage == 1
        stage = 2             # Attack finished; enter decay.
        ramp(0.8, 40 * pitch_ratio)
        return               # Wait for the decay ramp-end event.
    end

    if stage == 2
        stage = 3  # Decay finished; sustain at 0.8.
        return     # No new ramp: stay here until note-off.
    end

    if stage == 4
        stage = 0   # Release finished; return to idle.
        note_end()  # Tell firmware that the voice is finished.
    end
end
