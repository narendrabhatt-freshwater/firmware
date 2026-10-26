#include "board.h"
#include "channel.h"
#include "hardware.h"

#define DAC_ADDRESS 0x60u

/* ---- wait for an I2C event and expose bus errors without recovery -------- */

static void wait_i2c(uint32_t flag)
{
    for (;;) {
        uint32_t const status(I2C5->ISR);
        if (status & (I2C_ISR_NACKF | I2C_ISR_BERR | I2C_ISR_ARLO | I2C_ISR_OVR))
            fail();
        if (status & flag) return;
    }
}

/* ---- send one control-port byte ----------------------------------------- */

static void send_byte(uint8_t value)
{
    wait_i2c(I2C_ISR_TXIS);
    I2C5->TXDR = value;
}

/* ---- write one big-endian DAC register ---------------------------------- */

static void write_register(uint16_t address, uint16_t value)
{
    I2C5->CR2 = DAC_ADDRESS | (4u << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
    send_byte(uint8_t(address >> 8));
    send_byte(uint8_t(address));
    send_byte(uint8_t(value >> 8));
    send_byte(uint8_t(value));
    wait_i2c(I2C_ISR_STOPF);
    I2C5->ICR = I2C_ICR_STOPCF;
}

/* ---- initialize the DAC control bus and all four output paths ------------ */

void init_dac()
{
    RCC->APB1LENR |= RCC_APB1LENR_I2C5EN;
    (void)RCC->APB1LENR;
    GPIOC->OTYPER |= (1u << 10) | (1u << 11);
    configure_pin(GPIOC, 10, 2, 4);
    configure_pin(GPIOC, 11, 2, 4);
    I2C5->CR1 = 0;
    I2C5->TIMINGR = 0x60404e72;
    I2C5->CR1 = I2C_CR1_PE;
    write_pin(GPIOC, 7, false);
    delay_ms(2);
    write_pin(GPIOC, 7, true);
    delay_ms(6);
    write_register(0x0044, 0x0000);
    write_register(0x0040, 0x1031);
    write_register(0x0042, 0x0001);
    write_register(0x0048, 0x0001);
    write_register(0x0050, 0x0000);
    write_register(0x00c2, 0x0022);
    write_register(0x00c6, 0x0200);
    write_register(0x0044, 0x0001);
    write_register(0x00c0, 0x00ff);
    for (uint16_t address(0x00d0); address <= 0x00d6; address += 2)
        write_register(address, 0x0000);
    write_register(0x00e0, 0x0001);
}

constexpr unsigned dma_buffer_count(2);
static int32_t output[dma_buffer_count][SPB * 2] __attribute__((section(".dma_buffer"), aligned(4)));
static unsigned audio_read_index(0);
static int32_t control_samples[SPB * 2] __attribute__((section(".dma_buffer"), aligned(4)));
static int32_t const control_target[] = {
    int32_t(int64_t(INT32_MAX) * 32 * 50 / 10000),
    int32_t(int64_t(INT32_MAX) * 22 * 50 / 10000),
    int32_t(int64_t(INT32_MAX) * 32 * 50 / 10000)
};

static_assert(SPB == 48 && AUDIO_SAMPLE_RATE_HZ == 48000 && BUF_COUNT == 3);

/* ---- copy an existing mono block into one DMA buffer -------------------- */

static void copy_audio(unsigned slot)
{
    __DMB();
    for (unsigned i(0); i < SPB; ++i)
        output[slot][i * 2] = audio_buffers[audio_read_index][i];
    __DMB();
    audio_read_index = (audio_read_index + 1) % BUF_COUNT;
}

/* ---- refill the freed DMA buffer and request the next mono block -------- */

extern "C" void DMA1_Stream0_IRQHandler()
{
    uint32_t const flags(DMA1->LISR & 0x3d);
    DMA1->LIFCR = flags;
    if (flags & (DMA_LISR_DMEIF0 | DMA_LISR_TEIF0))
        fail();
    if (!(flags & DMA_LISR_TCIF0)) return;
    if (audio_read_index == audio_write_index) fail();
    unsigned const slot((DMA1_Stream0->CR & DMA_SxCR_CT) ? 0 : 1);
    copy_audio(slot);
    audio_completed = true;
}

/* ---- start audio double buffering or repeat the fixed control buffer ----- */

static void start_dma(unsigned port)
{
    SPI_TypeDef* i2s(port == 0 ? SPI1 : SPI2);
    DMA_Stream_TypeDef* stream(port == 0 ? DMA1_Stream0 : DMA1_Stream1);
    DMAMUX_Channel_TypeDef* mux(port == 0 ? DMAMUX1_Channel0 : DMAMUX1_Channel1);
    stream->CR = 0;
    while (stream->CR & DMA_SxCR_EN) {}
    mux->CCR = port == 0 ? 38 : 40;
    stream->FCR = 0;
    stream->PAR = reinterpret_cast<uintptr_t>(&i2s->TXDR);
    stream->M0AR = reinterpret_cast<uintptr_t>(port == 0 ? output[0] : control_samples);
    if (port == 0) stream->M1AR = reinterpret_cast<uintptr_t>(output[1]);
    stream->NDTR = SPB * 2;
    stream->CR = DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 | DMA_SxCR_MINC |
                 DMA_SxCR_DIR_0 | DMA_SxCR_CIRC | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    if (port == 0) stream->CR |= DMA_SxCR_DBM | DMA_SxCR_TCIE;
    DMA1->LIFCR = 0x3du << (port * 6);
    __DMB();
    stream->CR |= DMA_SxCR_EN;
    i2s->CFG1 |= SPI_CFG1_TXDMAEN;
    i2s->CR1 |= SPI_CR1_SPE;
}

/* ---- configure I2S and initialize fixed control samples ----------------- */

void init_audio()
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    RCC->APB1LENR |= RCC_APB1LENR_SPI2EN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;
    configure_pin(GPIOA, 4, 2, 5, 0, 2);
    configure_pin(GPIOA, 5, 2, 5, 0, 2);
    configure_pin(GPIOA, 7, 2, 5, 0, 2);
    configure_pin(GPIOC, 1, 2, 5, 0, 2);
    configure_pin(GPIOB, 12, 2, 5);
    configure_pin(GPIOA, 9, 2, 5);
    SPI1->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_1 |
                   SPI_I2SCFGR_DATLEN_1 | (4u << SPI_I2SCFGR_I2SDIV_Pos);
    SPI2->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_DATLEN_1;
    SPI2->CFG2 = SPI_CFG2_IOSWP;
    SPI1->IFCR = SPI_IFCR_UDRC | SPI_IFCR_TIFREC;
    SPI2->IFCR = SPI_IFCR_UDRC | SPI_IFCR_TIFREC;
    SPI1->IER = SPI_IER_UDRIE | SPI_IER_TIFREIE;
    SPI2->IER = SPI_IER_UDRIE | SPI_IER_TIFREIE;
    NVIC_SetPriority(SPI1_IRQn, 1);
    NVIC_SetPriority(SPI2_IRQn, 1);
    NVIC_EnableIRQ(SPI1_IRQn);
    NVIC_EnableIRQ(SPI2_IRQn);
    NVIC_SetPriority(DMA1_Stream0_IRQn, 1);
    NVIC_SetPriority(DMA1_Stream1_IRQn, 1);
    NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    audio_read_index = 0;
    for (unsigned i(0); i < SPB; ++i) {
        for (unsigned slot(0); slot < dma_buffer_count; ++slot)
            output[slot][i * 2 + 1] = control_target[0];
        control_samples[i * 2] = control_target[1];
        control_samples[i * 2 + 1] = control_target[2];
    }
}

/* ---- prefill both DMA buffers and start playback ------------------------ */

void start_audio()
{
    for (unsigned slot(0); slot < dma_buffer_count; ++slot) copy_audio(slot);
    start_dma(1);
    while (DMA1_Stream1->NDTR > SPB * 2 - 2) {}
    SPI2->CR1 |= SPI_CR1_CSTART;
    start_dma(0);
    SPI1->CR1 |= SPI_CR1_CSTART;
}

/* ---- halt if the fixed control transfer encounters a DMA error ---------- */

extern "C" void DMA1_Stream1_IRQHandler()
{ fail(); }

/* ---- halt on an audio I2S underrun or frame error ------------------------ */

extern "C" void SPI1_IRQHandler()
{ fail(); }

/* ---- halt on a fixed-control I2S underrun or frame error ------------------ */

extern "C" void SPI2_IRQHandler()
{ fail(); }
