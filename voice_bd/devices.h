// SPI  (/dev/spidev2.0)  host -> panel : display, LEDs, backlight
// UART (/dev/ttyS4)      panel -> host : buttons, switches, encoder
#ifdef __APPLE__
#define FRONTSPI_DEV "/dev/spidev2.0"
#define FRONTUART_DEV "/dev/ttyS4"
#define RS485_DEV "/dev/cu.usbserial-BG03CSYB"
#define CHANNEL1_DEV "/dev/cu.usbmodem1333101"
#define CHANNEL1_NAME "Channel Card BODY"
#define EFFECT_DEV "/dev/cu.usbmodemeffectcard1"
#define MIDI_PORT 0
#else // linux/CMI equivalents
#define FRONTSPI_DEV "/dev/spidev2.0"
#define FRONTUART_DEV "/dev/ttyS4"
#define RS485_DEV "/dev/ttyUSB0"
#define CHANNEL1_DEV "/dev/ttyACM0"
#define CHANNEL1_NAME "cafe:4031"
#define EFFECT_DEV "/dev/ttyACM1"
#define MIDI_PORT 1
#endif
// TODO CMI legacy keyboards
