#ifndef DMA_TEST_MAIN_H
#define DMA_TEST_MAIN_H
#include <stdint.h>
#include <stddef.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef struct { uint32_t Mode; } DMA_InitTypeDef;
typedef struct { uint32_t CR, NDTR; } DMA_Stream_TypeDef;
typedef struct DMA_HandleTypeDef DMA_HandleTypeDef;
struct DMA_HandleTypeDef {
  DMA_InitTypeDef Init;
  DMA_Stream_TypeDef *Instance;
  uint32_t tc_flag;
  void (*XferCpltCallback)(DMA_HandleTypeDef *);
  void (*XferM1CpltCallback)(DMA_HandleTypeDef *);
  void (*XferHalfCpltCallback)(DMA_HandleTypeDef *);
  void (*XferM1HalfCpltCallback)(DMA_HandleTypeDef *);
  void (*XferErrorCallback)(DMA_HandleTypeDef *);
  void (*XferAbortCallback)(DMA_HandleTypeDef *);
};
typedef struct { uint32_t CR1, CR3, RDR; } USART_TypeDef;
typedef struct {
  DMA_HandleTypeDef *hdmarx;
  DMA_HandleTypeDef *hdmatx;
  USART_TypeDef *Instance;
  struct { uint32_t Parity; } Init;
  uint8_t *pRxBuffPtr;
  uint32_t RxXferSize, RxXferCount, ReceptionType, ErrorCode, RxState;
  uint32_t gState;
} UART_HandleTypeDef;
#define DMA_CIRCULAR 1U
#define DMA_SxCR_CT (1U << 19)
#define DMA_SxCR_DBM (1U << 18)
#define UART_PARITY_NONE 0U
#define USART_CR1_PEIE (1U << 8)
#define USART_CR3_EIE 1U
#define USART_CR3_DMAR (1U << 6)
#define HAL_UART_ERROR_NONE 0U
#define HAL_UART_ERROR_DMA 16U
#define HAL_UART_RECEPTION_STANDARD 0U
#define HAL_UART_STATE_BUSY_RX 34U
#define HAL_UART_STATE_BUSY_TX 33U
#define HAL_UART_STATE_READY 32U
#define CLEAR_BIT(reg, bits) ((reg) &= ~(bits))
#define ATOMIC_CLEAR_BIT(reg, bits) CLEAR_BIT(reg, bits)
#define ATOMIC_SET_BIT(reg, bits) ((reg) |= (bits))
#define __HAL_DMA_GET_TC_FLAG_INDEX(dma) 1U
#define __HAL_DMA_GET_FLAG(dma, flag) ((void)(flag), (dma)->tc_flag)
#define __HAL_DMA_GET_COUNTER(dma) ((dma)->Instance->NDTR)
#define __HAL_UART_CLEAR_OREFLAG(uart) ((void)(uart))
static inline uint32_t __get_PRIMASK(void) { return 0; }
static inline void __disable_irq(void) {}
static inline void __set_PRIMASK(uint32_t state) { (void)state; }
static inline void __DMB(void) {}
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef *uart);
HAL_StatusTypeDef HAL_DMAEx_MultiBufferStart_IT(DMA_HandleTypeDef *dma, uint32_t source,
                                               uint32_t first, uint32_t second, uint32_t size);
uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t milliseconds);
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uart);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *uart, uint8_t *data, uint16_t count);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef *uart);
#endif
