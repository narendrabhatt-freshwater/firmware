# Application code is identical on STM32 and the simulator.
# Each target adds only its platform adapters in its own Makefile.
COMMON_APP_SOURCES := app/audio.cpp app/channel.cpp app/filter.cpp \
                      app/oscillator.cpp app/voice.cpp app/samples.cpp \
                      app/stream.cpp app/usb.cpp app/transport.cpp app/platform.cpp
