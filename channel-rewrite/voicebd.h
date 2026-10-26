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

#ifndef VOICEBD_H
#define VOICEBD_H

#include <cstdint>
#include <string>
#include <stdio.h>
#include <vector>

class voice_board_t
{
    int port;
    bool rs485_command(char const* command);

public:
    voice_board_t();
    ~voice_board_t();
    voice_board_t(voice_board_t const&) = delete;
    voice_board_t& operator=(voice_board_t const&) = delete;

    bool open();
    void close();
    bool note_on(uint8_t voice, uint16_t sample_id,
        uint8_t key, uint8_t velocity = 127);
    bool note_off(uint8_t voice);
    bool all_notes_off();
    bool load_sample(uint16_t sample_id, std::vector<int16_t> const& pcm);
    bool load_script(uint8_t channel, std::string const& path);
};

#endif
