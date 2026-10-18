/**
 * app_bmp.h - UART 로 BMP 수신 -> W25Q32 저장 -> Gray 변환 -> UART 송신
 */
#ifndef APP_BMP_H
#define APP_BMP_H

/* main() 의 while(1) 직전에 호출. 반환하지 않는다. (MX_SPI1_Init / MX_USARTx_Init 이후) */
void App_BMP_Run(void);

/* 명령 1개를 처리하고 반환 (RTOS 태스크나 다른 작업과 함께 쓰고 싶을 때) */
void App_BMP_Poll(void);

#endif /* APP_BMP_H */
