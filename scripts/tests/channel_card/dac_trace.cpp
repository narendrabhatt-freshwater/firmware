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
#include "audio_buffers.h"
#include "backend_model.h"
#include "application.h"
#include "transport.h"
#include "usb.h"
#include "audio_output.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
static void emit(int32_t *data,unsigned count) { assert(fwrite(data,4,count,stdout)==count); }
int main()
{
    init_usb_device(); init_application();
    int32_t data[192]={};
    for(unsigned ch=2;ch<=4;++ch) set_control_voltage_target(ch,0);
    render_control_voltage_slot(data,96,1,1); emit(data,192);
    render_control_voltage_channels(data,96,2,3); emit(data,192);
    for(int target : {100,-100,0}) {
        for(unsigned ch=2;ch<=4;++ch) set_control_voltage_target(ch,(int8_t)target);
        for(unsigned n=0;n<100;++n) {
            generate_audio_samples();
            for (unsigned i = 0; i < 96; ++i)
                data[i] = audio_output.samples[audio_output.read_index % BUF_COUNT][i];
            audio_output.read_index = (audio_output.read_index + 1) % (2u * BUF_COUNT);
            emit(data,96);
            for (unsigned i = 0; i < 96; ++i)
                data[i] = control_voltage_output.samples[control_voltage_output.read_index % BUF_COUNT][i];
            control_voltage_output.read_index = (control_voltage_output.read_index + 1) % (2u * BUF_COUNT);
            emit(data,96);
        }
    }
    return 0;
}
