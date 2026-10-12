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

#include "audio.h"
#include "channel.h"
#include "usb.h"
#ifndef LEGACY_API
#include "audio_output.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
static void emit(int32_t *data,unsigned count) { assert(fwrite(data,4,count,stdout)==count); }
int main()
{
    init_usb_protocol(); init_channel_card();
    int32_t data[192]={};
    for(unsigned ch=2;ch<=4;++ch) set_control_voltage_target(ch,0);
    render_control_voltage_slot(data,96,1,1); emit(data,192);
    render_control_voltage_channels(data,96,2,3); emit(data,192);
    for(int target : {100,-100,0}) {
        for(unsigned ch=2;ch<=4;++ch) set_control_voltage_target(ch,(int8_t)target);
        for(unsigned n=0;n<100;++n) {
#ifdef LEGACY_API
            render_audio_block(data,48);
#else
            render_dac_audio_frames(data,48);
#endif
            emit(data,96);
            render_control_voltage_channels(data,48,2,3); emit(data,96);
        }
    }
    return 0;
}
