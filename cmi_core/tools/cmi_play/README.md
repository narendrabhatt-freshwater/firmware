# cmi-play

`cmi-play` is the diagnostic and reference runner installed with `cmi_core`.

```sh
cmi-play doctor
cmi-play /path/to/project
cmi-play /path/to/project --sample /path/to/sample.wav \
  --sample-id 60 --root-hz 261.625565
```

It uses `cmi::Core::discover()` unless explicit `--rs485`, `--cdc`, `--audio`,
or `--midi` values are supplied. It uploads only user-provided files, reports
stage timings, watches `channel.be`, and provides an interactive control
prompt. No sample or wavetable assets are bundled or generated.

See the package [README](../../README.md) for setup and project layout.
