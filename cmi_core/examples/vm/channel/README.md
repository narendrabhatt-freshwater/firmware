# Channel script example

`channel.be` is the single production-ready example. Its active configuration
uses MIDI velocity for attack speed, a 500 ms decay to 20% sustain, a fast
voice-steal fade, and a manual note-off release. Commented alternatives in the
file show the available amplitude, pitch, and LED choices.

Load `channel.be` into all eight voices to use it with automatic MIDI voice
allocation. When a ninth note arrives, the oldest allocated voice fades for at
most 2 ms before promoting the pending note.

Load each Channel Card voice independently with
`cmi::Core::loadVoiceScript(voice, path)`. Voices may use different programs;
automatic MIDI input becomes active after all eight voice slots are loaded.

See [Channel scripting](../../../SCRIPTING.md) for the language, handler, input,
and native-function reference.
