def on_note_on(key, velocity)
    def recurse()
        recurse()
    end
    recurse()
end
def on_note_off() end
def on_ramp_end() end
