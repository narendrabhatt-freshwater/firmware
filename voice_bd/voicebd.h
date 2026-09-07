/*
                                   __
                               ___  \  \
                          ___  \  \  \  \    _______
                     ___  \  \  \  \  \__\__/_____  \
                     \  \  \  \  \__\_______      \__\____
                      \  \  \  \     ___ \  \________  \__/
                       \  \  \__\___/_  \ \___/   \  \___
                        \  \    ____  \  \________ \ ___/
                         \__\__/ \  \  \____/  \  \
                                  \  \     ___  \  \___
                                   \__\___/ \  \ \____/
                                           \  \
                                              \  \___
                 __                 _          \ ___/  _
                / _|_ __  ___  ___ | |____      ____ _| |_  ___  _ __
               | |_| '_ |/ _ \/ __|| '_ \ \ /\ / / _` | __|/ _ \| '__|
               |  _| |  |  __/\__ \| | | \ V  V / (_| | |_|  __/| |
               |_| |_|   \___||___/|_| |_|\_/\_/ \__,_|\__|\___||_|

            (C) 2 0 2 6   F r e s h w a t e r   I n s t r u m e n t s
*/

#ifndef FRESHWATER_VOICE_BOARD_H
#define FRESHWATER_VOICE_BOARD_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class voice_board_error_t : uint8_t {
    ok = 0,
    invalid_argument,
    not_connected,
    device_not_found,
    device_ambiguous,
    io_error,
    timeout,
    bad_reply,
    bec_error,
    sample_error,
    audio_error
};

struct voice_board_result_t {
    voice_board_error_t code = voice_board_error_t::ok;
    std::string message;

    /* ---- test whether the board result succeeded ------------------------- */

    bool ok() const { return code == voice_board_error_t::ok; }
    /* ---- convert the board result to boolean ----------------------------- */

    explicit operator bool() const { return ok(); }
};

/* Reserved for per-channel controls. These values are not sent yet. */
struct controls_t {
    uint8_t pb = 64;               /* Pitch bend, centred at 64. */
    uint8_t af = 0;                /* Aftertouch. */
    std::vector<uint8_t> switches; /* Switch values. */
};

struct voice_board_config_t {
    /* Program loaded into all eight voices by open(). */
    std::string bec_file = "channel.bec";
    /* USB-to-RS485 device, for example /dev/cu.usbserial-XXXX. */
    std::string rs485_port = "/dev/cu.usbserial-BG03CSYB";
    /* USB port used to load the BEC and sample ATTACK data. */
    std::string upload_usb_port = "/dev/cu.usbmodem13103";
    /* RtAudio device used for the USB BODY stream. */
    std::string stream_usb_port = "Channel Card BODY";
    /* RS485 baud rate. */
    uint32_t rs485_baud = 921600;
    /* Output attenuation in dB: 0 is loudest. */
    uint8_t initial_attenuation_db = 0;
};

class voice_board_t {
private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;

public:
    static constexpr uint8_t voice_count = 8;
    voice_board_t();
    ~voice_board_t();
    voice_board_t(voice_board_t&&) noexcept;
    voice_board_t& operator=(voice_board_t&&) noexcept;
    voice_board_t(voice_board_t const&) = delete;
    voice_board_t& operator=(voice_board_t const&) = delete;
    /* Open the ports and load the BEC. */
    voice_board_result_t open(voice_board_config_t const& config);
    /* Silence the card and close the ports. */
    voice_board_result_t close();
    bool is_open() const;
    /* Load 48 kHz mono signed-16 PCM into a sample slot. */
    voice_board_result_t load_sample(uint16_t sample_id,
        std::vector<int16_t> const& pcm,
        uint32_t source_sample_rate_hz = 48000,
        double root_pitch_hz = 261.625565);
    /* Start a note on one voice. */
    voice_board_result_t note_on(uint8_t voice_id, uint16_t sample_id,
        uint8_t midi_key, uint8_t velocity = 127);
    /* Release one voice. */
    voice_board_result_t note_off(uint8_t voice_id);
    /* Release all voices. */
    voice_board_result_t all_notes_off();
    /* Set output attenuation in dB (0..127). */
    voice_board_result_t set_attenuation(uint8_t attenuation_db);
};

#endif /* FRESHWATER_VOICE_BOARD_H */
