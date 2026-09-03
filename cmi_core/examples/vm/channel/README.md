# Channel script examples

These programs are small production-ready examples of one behavior each. They
use handlers, persistent stage state, ramps, and voice cleanup without using
`input()` or any low-level input constants.

- `attack_2ms.be` — 2 ms attack, held level, and 50 ms release.
- `gate.be` — immediate full-level gate with a 20 ms release.
- `pluck.be` — 2 ms attack followed by a 500 ms automatic decay.
- `voice_steal.be` — fade the current note for at most 2 ms, promote the
  pending note, and start it with a 2 ms attack.

Load `voice_steal.be` into all eight voices to use it with automatic MIDI voice
allocation. When a ninth note arrives, the oldest allocated voice performs the
documented steal transition.

Load each Channel Card voice independently with
`cmi::Core::loadVoiceScript(voice, path)`. Voices may use different programs;
automatic MIDI input becomes active after all eight voice slots are loaded.

See [Channel scripting](../../../SCRIPTING.md) for the language, handler, input,
and native-function reference.
