# Freshwater Berry compiler

This folder is self-contained. Zip it, send it to a teammate, and they can
build the compiler without the rest of the firmware repository.

## Build and use

You need GNU Make and a C compiler (Clang or GCC). On macOS these are included
in the Xcode Command Line Tools. On Windows, use an MSYS2 MinGW environment
with Make and GCC installed, and run these commands in its shell.

Unzip the folder, open a terminal in it, and run:

```sh
make
./berry examples/channel_envelope.be -o channel_envelope.bec
```

On Windows the executable is `berry.exe`. Replace the example path with your
own `.be` script. The `.bec` output includes the firmware upload header,
ABI version, and checksum, ready to upload to the Channel Card.

`make clean` removes the Make build and executable. CMake is also supported:

```sh
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --target berry --parallel
```

## Included source

- `berry.c`: Freshwater compiler entry point.
- `vendor/berry/`: Berry engine source, generated tables, license, and provenance.
- `config/`: host compiler configuration.
- `shared/`: Freshwater source preprocessing and bytecode container definitions.
- `examples/`: a sample Channel Card script.

Everything needed to build is included; the build downloads nothing.
Use a compiler from the same firmware revision as the target device.

For maintainers: `vendor/berry`, `config/berry_conf.h`, and
`shared/src/berry_runtime_modtab.c` are snapshots from `cmi_core/runtime`.
The other shared C sources and `freshwater` headers come from
`cmi_core/shared/vm`; `script/script_runtime.h` comes from the runtime include
directory. Refresh these copies when the corresponding firmware sources
change, and qualify the compiler against the runtime tests.
