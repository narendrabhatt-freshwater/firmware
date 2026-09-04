# Channel script example

`channel.be` is the single production-ready example. Its active configuration
uses MIDI velocity for attack speed, a 500 ms decay to 90% sustain, a 5 ms
voice-steal fade, and a manual note-off release. It is sample-only by default;
commented alternatives show amplitude, pitch, oscillator, routing, and LED
choices. Oscillator examples require the user to upload their own wavetable.

Load `channel.be` into all eight voices to use it with automatic MIDI voice
allocation. When a ninth note arrives, the oldest allocated voice fades for at
most 2 ms before promoting the pending note.

Load each Channel Card voice independently with
`cmi::Core::loadVoiceScript(voice, path)`. Voices may use different programs;
automatic MIDI input becomes active after all eight voice slots are loaded.

See [Channel scripting](../../../SCRIPTING.md) for the language, handler, input,
and native-function reference.
