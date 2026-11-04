#ifndef LCD_TEST_H
#define LCD_TEST_H

#include "main.h"

/* Board LCD test for the STM32F746G-DISCO 480x272 panel. Shows the 24-bit
   BMP stored at Flash address 0 as an 8-bit gray preview (a gray ramp when
   none is stored). The USER button redraws it after a new image is stored. */
HAL_StatusTypeDef LcdTest_Init(void);
int LcdTest_ShowFlashBmp(QSPI_HandleTypeDef *flash);
void LcdTest_Poll(QSPI_HandleTypeDef *flash);

#endif
