#include "audio_output.h"
#include "stm32/i2s.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

// Exercise the real output driver with modeled DMA address registers.
// Mach-O cannot use the board's linker section; its placement is checked in the ELF.
#define __attribute__(attributes)
#include "../../../channel_card/app/audio_i2s.cpp"
#undef __attribute__

AudioBuffers audio_output{}, control_voltage_output{};
TestSpi test_spi1{}, test_spi2{};
TestTimer test_timer{};
static DMA_Stream_TypeDef dma_streams[2]{};
static DMA_HandleTypeDef dma_handles[2]{};
I2S_HandleTypeDef hi2s1{}, hi2s2{};
static unsigned notifications, rendered[2];

void signal_interrupt() { ++notifications; }
[[noreturn]] void Error_Handler() { std::abort(); }
void set_audio_output_diagnostics(const AudioOutputDiagnostics *) {}

static unsigned queued(const AudioBuffers &buffers)
{
    return (buffers.write_index + 2u * BUF_COUNT - buffers.read_index) % (2u * BUF_COUNT);
}

static AudioBuffers &buffers_for(unsigned port)
{
    return port ? control_voltage_output : audio_output;
}

HAL_StatusTypeDef HAL_DMAEx_MultiBufferStart_IT(DMA_HandleTypeDef *dma, uintptr_t first,
                                               uintptr_t peripheral, uintptr_t second, uint32_t words)
{
    unsigned port = dma == &dma_handles[1];
    AudioBuffers &buffers = buffers_for(port);
    auto *i2s = static_cast<I2S_HandleTypeDef *>(dma->Parent);
    assert(words == AUDIO_BUFFER_WORDS); // One 48-frame block, independent of BUF_COUNT.
    assert(first == reinterpret_cast<uintptr_t>(buffers.samples[buffers.read_index % BUF_COUNT]));
    assert(second == reinterpret_cast<uintptr_t>(buffers.samples[(buffers.read_index + 1) % BUF_COUNT]));
    assert(peripheral == reinterpret_cast<uintptr_t>(&i2s->Instance->TXDR));
    assert(queued(buffers) == BUF_COUNT);
    assert(!dma->XferHalfCpltCallback && !dma->XferM1HalfCpltCallback);
    assert(dma->XferCpltCallback && dma->XferM1CpltCallback && dma->XferErrorCallback);
    if (port) assert(SPI1->CR1 & SPI_CR1_CSTART); // Preserve master-first startup.
    dma->Instance->M0AR = first;
    dma->Instance->M1AR = second;
    dma->Instance->NDTR = words;
    dma->Instance->CR |= DMA_SxCR_DBM;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DMAEx_ChangeMemory(DMA_HandleTypeDef *dma, uintptr_t address,
                                        HAL_DMA_MemoryTypeDef memory)
{
    const bool playing_memory1 = (dma->Instance->CR & DMA_SxCR_CT) != 0;
    assert(memory == (playing_memory1 ? MEMORY0 : MEMORY1));
    if (memory == MEMORY0) dma->Instance->M0AR = address;
    else dma->Instance->M1AR = address;
    return HAL_OK;
}

void render_control_voltage_slot(int32_t *output, uint32_t frames, uint8_t, uint8_t slot)
{
    for (unsigned i = 0; i < frames; ++i) output[2 * i + slot] = 123;
}

static void render(int32_t *output, uint32_t frames, unsigned port)
{
    AudioBuffers &buffers = buffers_for(port);
    if (queued(buffers)) assert(output == buffers.samples[buffers.write_index % BUF_COUNT]); // Generate directly in DMA storage.
    for (unsigned i = 0; i < frames; ++i) {
        output[2 * i] = 1000 + rendered[port];
        output[2 * i + 1] = -static_cast<int32_t>(1000 + rendered[port]++);
    }
}
void generate_audio_samples()
{
    for (unsigned port = 0; port < 2; ++port) {
        auto &buffers = buffers_for(port);
        if (queued(buffers) == BUF_COUNT) continue;
        render(buffers.samples[buffers.write_index % BUF_COUNT], AUDIO_BUFFER_FRAMES, port);
        buffers.write_index = (buffers.write_index + 1) % (2u * BUF_COUNT);
    }
}
void render_control_voltage_channels(int32_t *output, uint32_t frames, uint8_t, uint8_t)
{
    render(output, frames, 1);
}

static void complete_block(unsigned port)
{
    auto &dma = dma_handles[port];
    dma.Instance->CR ^= DMA_SxCR_CT; // Hardware switches addresses before raising TC.
    unsigned before = notifications;
    if (dma.Instance->CR & DMA_SxCR_CT) dma.XferM1CpltCallback(&dma);
    else dma.XferCpltCallback(&dma);
    assert(notifications == before + 1);
}

static void check_samples(const int32_t *samples, unsigned start)
{
    for (unsigned i = 0; i < AUDIO_BUFFER_FRAMES; ++i) {
        assert(samples[2 * i] == static_cast<int32_t>(1000 + start + i));
        assert(samples[2 * i + 1] == -static_cast<int32_t>(1000 + start + i));
    }
}

int main()
{
    hi2s1.Instance = SPI1; hi2s1.hdmatx = &dma_handles[0];
    hi2s2.Instance = SPI2; hi2s2.hdmatx = &dma_handles[1];
    dma_handles[0].Instance = &dma_streams[0]; dma_handles[0].Parent = &hi2s1;
    dma_handles[1].Instance = &dma_streams[1]; dma_handles[1].Parent = &hi2s2;
    start_audio_playback();
    assert(rendered[0] == 0 && rendered[1] == BUF_COUNT * AUDIO_BUFFER_FRAMES);
    for (auto &block : audio_output.samples) {
        for (unsigned i = 0; i < AUDIO_BUFFER_FRAMES; ++i)
            assert(block[2 * i] == 0 && block[2 * i + 1] == 123);
    }
    for (unsigned completion = 0; completion < 100; ++completion) {
        for (unsigned port = 0; port < 2; ++port) {
            AudioBuffers &buffers = buffers_for(port);
            unsigned finished = buffers.read_index % BUF_COUNT;
            int32_t previous[BUF_COUNT][AUDIO_BUFFER_WORDS];
            std::memcpy(previous, buffers.samples, sizeof previous);
            unsigned start = rendered[port];
            complete_block(port);
            assert(rendered[port] == start); // ISR only advances addresses and wakes main.
            assert(std::memcmp(previous, buffers.samples, sizeof previous) == 0);
            assert(queued(buffers) == BUF_COUNT - 1);
            generate_audio_samples();
            assert(rendered[port] == start + AUDIO_BUFFER_FRAMES && queued(buffers) == BUF_COUNT);
            check_samples(buffers.samples[finished], start);
            for (unsigned slot = 0; slot < BUF_COUNT; ++slot) {
                if (slot != finished)
                    assert(std::memcmp(previous[slot], buffers.samples[slot], sizeof previous[slot]) == 0);
            }
        }
    }
    // Several blocks can complete before main; refill only those slots in FIFO order.
    unsigned first = audio_output.read_index % BUF_COUNT, start = rendered[0];
    for (unsigned i = 0; i < BUF_COUNT - 1; ++i) complete_block(0);
    assert(queued(audio_output) == 1);
    int32_t playing[AUDIO_BUFFER_WORDS];
    std::memcpy(playing, audio_output.samples[audio_output.read_index % BUF_COUNT], sizeof playing);
    for (unsigned i = 0; i < BUF_COUNT - 1; ++i) {
        generate_audio_samples();
        assert(queued(audio_output) == i + 2);
        assert(rendered[0] == start + (i + 1) * AUDIO_BUFFER_FRAMES);
    }
    assert(std::memcmp(playing, audio_output.samples[audio_output.read_index % BUF_COUNT], sizeof playing) == 0);
    for (unsigned i = 0; i < BUF_COUNT - 1; ++i)
        check_samples(audio_output.samples[(first + i) % BUF_COUNT], start + i * AUDIO_BUFFER_FRAMES);

    // The existing I2S2 error restart resumes at the current ring position.
    unsigned restarts = g_i2s2_err_restarts;
    dma_handles[1].XferErrorCallback(&dma_handles[1]);
    assert(g_i2s2_err_restarts == restarts + 1);
    complete_block(1);
    generate_audio_samples();

    // Detect exhaustion and a wrong active address rather than freeing live data.
    for (unsigned fault = 0; fault < 2; ++fault) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            if (fault == 0) {
                for (unsigned i = 0; i < BUF_COUNT; ++i) complete_block(0);
            } else {
                dma_streams[0].M0AR = dma_streams[0].M1AR = 0;
                complete_block(0);
            }
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    assert(g_i2s1_fill_late == 0 && g_i2s2_fill_late == 0);
}
