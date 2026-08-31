# Simple envelope
# state[0]: 0=idle, 1=attack, 2=decay, 3=sustain, 4=release

def on_note_on()
    start_note()       # Immediately make the received note active.
    set_amplitude(0)  # Restart its envelope from silence.
    state_set(0, 1)   # Enter attack.
    ramp(1, 20)       # 0.0 -> 1.0 at slope 20 (50 ms).
end

def on_note_off()
    state_set(0, 4)  # Enter release.
    ramp(0, 2)     # Current amplitude -> 0.0 at slope 200.
end

def on_ramp_end()
    var stage = state_get(0)  # Which ramp just finished?

    if stage == 1
        state_set(0, 2)  # Attack finished; enter decay.
        ramp(0.8, 40)    # 1.0 -> 0.8 at slope 40 (5 ms).
        return           # Wait for the decay ramp-end event.
    end

    if stage == 2
        state_set(0, 3)  # Decay finished; sustain at 0.8.
        return           # No new ramp: stay here until note-off.
    end

    if stage == 4
        state_set(0, 0)  # Release finished; return to idle.
        note_end()       # Tell firmware that the voice is finished.
    end
end
