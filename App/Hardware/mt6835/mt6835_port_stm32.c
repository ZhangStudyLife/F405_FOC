#include "mt6835_port_stm32.h"
#include "mt6835.h"
#include "foc.h"
#include "spi.h"
#include <math.h>

static const uint8_t s_tx[6] = {0xa0, 0x03, 0, 0, 0, 0};
static uint8_t s_rx[6];
volatile float mt6835_angle_deg = NAN, mt6835_sample_delay;
volatile uint32_t mt6835_errors;

/* Initialization only: register 0x001 is user RAM, not a device ID. */
static int probe_transfer(uint8_t command, uint8_t value)
{
    uint8_t tx[3] = {command, 1u, value}, rx[3];
    GPIOA->BSRR = GPIO_PIN_0 << 16;
    HAL_StatusTypeDef result = HAL_SPI_TransmitReceive(&hspi3, tx, rx, 3u, 1u);
    GPIOA->BSRR = GPIO_PIN_0;
    HAL_Delay(1u);
    return result == HAL_OK ? rx[2] : -1;
}

bool mt6835_init(void)
{
    GPIOA->BSRR = GPIO_PIN_0;
    HAL_Delay(20u);
    int original = probe_transfer(0x30, 0u);
    if (original < 0) return false;
    int written = probe_transfer(0x60, 0x5a);
    int readback = probe_transfer(0x30, 0u);
    int restored = probe_transfer(0x60, (uint8_t)original);
    if (written < 0 || readback != 0x5a || restored < 0) return false;

    __HAL_RCC_DMA1_CLK_ENABLE();
    DMA1_Stream0->PAR = (uint32_t)&SPI3->DR;
    DMA1_Stream0->M0AR = (uint32_t)s_rx;
    DMA1_Stream0->CR = DMA_SxCR_MINC | DMA_SxCR_PL_1 |
                          DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA1_Stream5->PAR = (uint32_t)&SPI3->DR;
    DMA1_Stream5->M0AR = (uint32_t)s_tx;
    DMA1_Stream5->CR = DMA_SxCR_MINC | DMA_SxCR_PL_1 | DMA_SxCR_DIR_0;
    (void)SPI3->DR;
    (void)SPI3->SR;
    SPI3->CR1 |= SPI_CR1_SPE;
    SPI3->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    return true;
}

void mt6835_start(void)
{
    /* A transfer must finish within the preceding 50 us period. */
    if ((DMA1_Stream0->CR | DMA1_Stream5->CR) & DMA_SxCR_EN) {
        mt6835_angle_deg = NAN;
        mt6835_errors++;
        return;
    }
    DMA1->LIFCR = 0x3du;      /* Stream 0 only. */
    DMA1->HIFCR = 0xf40u;     /* Stream 5 only; UART uses stream 6. */
    DMA1_Stream0->NDTR = sizeof s_rx;
    DMA1_Stream5->NDTR = sizeof s_tx;
    /* CS is only a proxy for angle time; internal sensor latency is uncalibrated.
       Timer down-count is guaranteed here by the ADC rank sequence. */
    mt6835_sample_delay = (8400.0f - (float)TIM8->CNT - FOC_HOLD_TICKS) / 168e6f;
    GPIOA->BSRR = GPIO_PIN_0 << 16;
    DMA1_Stream0->CR |= DMA_SxCR_EN;
    DMA1_Stream5->CR |= DMA_SxCR_EN;
}

void mt6835_finish(void)
{
    uint32_t flags = DMA1->LISR;
    DMA1->LIFCR = 0x3du;
    /* >= half SCK period between the last edge and CS rising. */
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
    GPIOA->BSRR = GPIO_PIN_0;
    if ((flags & (DMA_LISR_TCIF0 | DMA_LISR_TEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_FEIF0)) != DMA_LISR_TCIF0) {
        DMA1_Stream0->CR &= ~DMA_SxCR_EN;
        DMA1_Stream5->CR &= ~DMA_SxCR_EN;
        while ((DMA1_Stream0->CR | DMA1_Stream5->CR) & DMA_SxCR_EN) {}
        (void)SPI3->DR;
        (void)SPI3->SR;
        mt6835_angle_deg = NAN;
    } else {
        mt6835_angle_deg = mt6835_decode(s_rx);
    }
    if (isnan(mt6835_angle_deg)) mt6835_errors++;
}

void mt6835_stop(void)
{
    DMA1_Stream0->CR &= ~DMA_SxCR_EN;
    DMA1_Stream5->CR &= ~DMA_SxCR_EN;
    while ((DMA1_Stream0->CR | DMA1_Stream5->CR) & DMA_SxCR_EN) {}
    SPI3->CR1 &= ~SPI_CR1_SPE;
    (void)SPI3->DR; (void)SPI3->SR;
    GPIOA->BSRR = GPIO_PIN_0;
    DMA1->LIFCR = 0x3du; DMA1->HIFCR = 0xf40u;
    HAL_NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);
    SPI3->CR1 |= SPI_CR1_SPE;
}
