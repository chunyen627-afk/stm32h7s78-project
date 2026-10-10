/* UART4 接收：中斷收進環形緩衝，主迴圈逐行取出。
 *
 * 只用 HAL 做初始化（鮑率要從 UART4 的核心時脈算，交給 HAL 不會算錯），
 * 接收不走 HAL_UART_Receive_IT —— 那套要預先給長度，而 SimHub 送的是
 * 不定長的一行一行文字。中斷處理常式直接讀 RDR。
 *
 * 腳位：PD1 = TX、PD0 = RX、AF8（board-notes 9.2）。 */
#include "serial.h"
#include "main.h"

static UART_HandleTypeDef s_uart;

#define RX_SIZE 2048u                  /* 2 的次方；一行最多約 400 字，留足空間 */
static volatile uint8_t  s_rx[RX_SIZE];
static volatile uint32_t s_head;       /* 中斷寫 */
static uint32_t          s_tail;       /* 主迴圈讀 */

volatile uint32_t g_serial_rx_bytes;
volatile uint32_t g_serial_rx_overrun;
volatile uint32_t g_serial_line_dropped;

void HAL_UART_MspInit(UART_HandleTypeDef *h)
{
    GPIO_InitTypeDef g = {0};

    if (h->Instance != UART4) { return; }
    __HAL_RCC_UART4_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    g.Pin       = GPIO_PIN_0 | GPIO_PIN_1;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF8_UART4;
    HAL_GPIO_Init(GPIOD, &g);
}

bool serial_init(uint32_t baud)
{
    s_uart.Instance            = UART4;
    s_uart.Init.BaudRate       = baud;
    s_uart.Init.WordLength     = UART_WORDLENGTH_8B;
    s_uart.Init.StopBits       = UART_STOPBITS_1;
    s_uart.Init.Parity         = UART_PARITY_NONE;
    s_uart.Init.Mode           = UART_MODE_TX_RX;
    s_uart.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
    s_uart.Init.OverSampling   = UART_OVERSAMPLING_16;
    s_uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    s_uart.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    if (HAL_UART_Init(&s_uart) != HAL_OK) { return false; }

    UART4->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
    HAL_NVIC_SetPriority(UART4_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(UART4_IRQn);
    return true;
}

void UART4_IRQHandler(void)
{
    uint32_t isr = UART4->ISR;

    /* 溢位、雜訊、框架錯誤都要清，不清的話 ORE 會讓 RXNE 永遠不再觸發，
     * 症狀是「收了一陣子之後再也收不到」。 */
    if (isr & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE)) {
        if (isr & USART_ISR_ORE) { g_serial_rx_overrun++; }
        UART4->ICR = USART_ICR_ORECF | USART_ICR_NECF | USART_ICR_FECF;
    }
    while (UART4->ISR & USART_ISR_RXNE_RXFNE) {
        uint8_t  c    = (uint8_t)UART4->RDR;
        uint32_t next = (s_head + 1u) & (RX_SIZE - 1u);

        g_serial_rx_bytes++;
        if (next != s_tail) {          /* 滿了就丟最新的，主迴圈追上之前不覆蓋 */
            s_rx[s_head] = c;
            s_head = next;
        }
    }
}

int serial_read_line(char *buf, int cap)
{
    static int  len;                   /* 這一行目前累積到哪 */
    static bool overflow;

    while (s_tail != s_head) {
        char c = (char)s_rx[s_tail];

        s_tail = (s_tail + 1u) & (RX_SIZE - 1u);
        if (c == '\r') { continue; }   /* 接受 CRLF */
        if (c == '\n') {
            int n = len;
            bool bad = overflow;

            len = 0;
            overflow = false;
            if (bad) { g_serial_line_dropped++; continue; }
            buf[n] = '\0';
            return n;
        }
        if (len < cap - 1) {
            buf[len++] = c;
        } else {
            overflow = true;
        }
    }
    return -1;
}
