def on_note_on(key)
    def recurse()
        recurse()
    end
    recurse()
end
def on_note_off(has_pending) end
def on_ramp_end() end
