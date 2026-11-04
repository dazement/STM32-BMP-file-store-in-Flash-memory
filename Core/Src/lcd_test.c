#include "lcd_test.h"
#include "bmp_gray.h"
#include "debug_log.h"
#include "flash_test.h"
#include <string.h>

/* RK043FN48H panel timings, as in ST's STM32746G-Discovery BSP. */
#define LCD_WIDTH 480U
#define LCD_HEIGHT 272U
#define LCD_HSYNC 41U
#define LCD_HBP 13U
#define LCD_HFP 32U
#define LCD_VSYNC 10U
#define LCD_VBP 2U
#define LCD_VFP 2U
#define LTDC_PIXEL_FORMAT_L8 5U
#define CHUNK_PIXELS 1024U
#define IMAGE_LIMIT 0x3F0000U

/* One CLUT index per pixel (L8): 130,560 bytes of internal SRAM, so the
   SDRAM stays unused. It holds a downscaled preview, not the image. */
static uint8_t frame[LCD_HEIGHT][LCD_WIDTH] __attribute__((aligned(32)));
static uint8_t bgr[CHUNK_PIXELS * 3U];
static uint8_t gray[CHUNK_PIXELS];
static int lcd_ready;
static GPIO_PinState button_idle, button_last;

static uint32_t get_u32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* Pins as in ST's BSP_LCD_MspInit, plus the USER button. */
static void init_pins(void)
{
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOI_CLK_ENABLE();
  __HAL_RCC_GPIOJ_CLK_ENABLE();
  __HAL_RCC_GPIOK_CLK_ENABLE();
  GPIO_InitTypeDef pin = {0};
  pin.Mode = GPIO_MODE_AF_PP;
  pin.Pull = GPIO_NOPULL;
  pin.Speed = GPIO_SPEED_FREQ_HIGH;
  pin.Alternate = GPIO_AF14_LTDC;
  pin.Pin = GPIO_PIN_4;                                          /* B0 */
  HAL_GPIO_Init(GPIOE, &pin);
  pin.Pin = GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15; /* VSYNC HSYNC CLK R0 */
  HAL_GPIO_Init(GPIOI, &pin);
  pin.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 |
            GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 |
            GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
  HAL_GPIO_Init(GPIOJ, &pin);                                    /* R1-R7 G0-G4 B1-B3 */
  pin.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
            GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
  HAL_GPIO_Init(GPIOK, &pin);                                    /* G5-G7 B5-B7 DE */
  pin.Pin = GPIO_PIN_12;                                         /* B4 */
  pin.Alternate = GPIO_AF9_LTDC;
  HAL_GPIO_Init(GPIOG, &pin);

  pin.Mode = GPIO_MODE_OUTPUT_PP;
  pin.Alternate = 0;
  pin.Pin = GPIO_PIN_12;                                         /* LCD_DISP */
  HAL_GPIO_Init(GPIOI, &pin);
  pin.Pin = GPIO_PIN_3;                                          /* LCD_BL_CTRL */
  HAL_GPIO_Init(GPIOK, &pin);
  pin.Mode = GPIO_MODE_INPUT;
  pin.Pin = GPIO_PIN_11;                                         /* USER button */
  HAL_GPIO_Init(GPIOI, &pin);
}

HAL_StatusTypeDef LcdTest_Init(void)
{
  init_pins();
  /* 1 MHz PLL input x 192 / 5 / 4 = 9.6 MHz pixel clock (ST BSP values). */
  RCC_PeriphCLKInitTypeDef clock = {0};
  clock.PeriphClockSelection = RCC_PERIPHCLK_LTDC;
  clock.PLLSAI.PLLSAIN = 192;
  clock.PLLSAI.PLLSAIR = 5;
  clock.PLLSAIDivR = RCC_PLLSAIDIVR_4;
  if (HAL_RCCEx_PeriphCLKConfig(&clock) != HAL_OK)
    return HAL_ERROR;
  __HAL_RCC_LTDC_CLK_ENABLE();

  /* Register values HAL_LTDC_Init and LTDC_SetConfig would write. */
  const uint32_t hbp = LCD_HSYNC + LCD_HBP - 1U;
  const uint32_t vbp = LCD_VSYNC + LCD_VBP - 1U;
  LTDC->SSCR = ((LCD_HSYNC - 1U) << 16) | (LCD_VSYNC - 1U);
  LTDC->BPCR = (hbp << 16) | vbp;
  LTDC->AWCR = ((hbp + LCD_WIDTH) << 16) | (vbp + LCD_HEIGHT);
  LTDC->TWCR = ((hbp + LCD_WIDTH + LCD_HFP) << 16) | (vbp + LCD_HEIGHT + LCD_VFP);
  LTDC->BCCR = 0;                /* black background */
  LTDC->GCR = LTDC_GCR_LTDCEN;   /* HSYNC, VSYNC, DE active low; clock not inverted */

  LTDC_Layer_TypeDef *layer = LTDC_Layer1;
  layer->WHPCR = ((hbp + LCD_WIDTH) << 16) | (hbp + 1U);
  layer->WVPCR = ((vbp + LCD_HEIGHT) << 16) | (vbp + 1U);
  layer->PFCR = LTDC_PIXEL_FORMAT_L8;
  layer->CACR = 255U;
  layer->DCCR = 0U;
  layer->BFCR = (4U << 8) | 5U;  /* blend by constant alpha */
  layer->CFBAR = (uint32_t)(uintptr_t)frame;
  layer->CFBLR = (LCD_WIDTH << 16) | (LCD_WIDTH + 3U);  /* pitch | line bytes + 3 */
  layer->CFBLNR = LCD_HEIGHT;
  /* The CLUT may be written while the layer is disabled: index i is gray i. */
  for (uint32_t i = 0; i < 256U; ++i)
    layer->CLUTWR = (i << 24) | (i << 16) | (i << 8) | i;
  layer->CR = LTDC_LxCR_CLUTEN | LTDC_LxCR_LEN;
  LTDC->SRCR = LTDC_SRCR_IMR;

  HAL_GPIO_WritePin(GPIOI, GPIO_PIN_12, GPIO_PIN_SET);  /* display on */
  HAL_GPIO_WritePin(GPIOK, GPIO_PIN_3, GPIO_PIN_SET);   /* backlight on */
  button_idle = button_last = HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_11);
  lcd_ready = 1;
  return HAL_OK;
}

static void draw_ramp(void)
{
  for (uint32_t y = 0; y < LCD_HEIGHT; ++y)
    for (uint32_t x = 0; x < LCD_WIDTH; ++x)
      frame[y][x] = (uint8_t)(x * 255U / (LCD_WIDTH - 1U));
}

/* Nearest-neighbour samples of one stored BMP row, converted by the same
   formula as the UART Gray output. */
static int draw_row(QSPI_HandleTypeDef *flash, const BmpGrayInfo *info,
                    uint32_t address, uint8_t *line, uint32_t width)
{
  uint32_t x = 0;
  while (x < width)
  {
    uint32_t first = (uint32_t)((uint64_t)x * info->width / width);
    uint32_t count = info->width - first;
    if (count > CHUNK_PIXELS)
      count = CHUNK_PIXELS;
    if (FlashTest_Read(flash, address + first * 3U, bgr, count * 3U) != HAL_OK)
      return 0;
    BmpGray_Convert(bgr, count, gray, 8);
    for (; x < width; ++x)
    {
      uint32_t source = (uint32_t)((uint64_t)x * info->width / width);
      if (source >= first + count)
        break;
      line[x] = gray[source - first];
    }
  }
  return 1;
}

int LcdTest_ShowFlashBmp(QSPI_HandleTypeDef *flash)
{
  if (!lcd_ready)
    return 0;
  uint32_t start = HAL_GetTick();
  uint8_t header[54];
  BmpGrayInfo info;
  if (FlashTest_Read(flash, 0, header, sizeof(header)) != HAL_OK ||
      get_u32(header + 2) > IMAGE_LIMIT ||
      !BmpGray_Parse(header, get_u32(header + 2), 8, &info))
  {
    draw_ramp();
    DebugLog_Write("LCD: no BMP in Flash, gray ramp shown");
    return 0;
  }
  /* Fit inside 480x272, keeping the aspect ratio, centred on black. */
  uint32_t width = LCD_WIDTH;
  uint32_t height = (uint32_t)((uint64_t)info.rows * LCD_WIDTH / info.width);
  if (height > LCD_HEIGHT)
  {
    height = LCD_HEIGHT;
    width = (uint32_t)((uint64_t)info.width * LCD_HEIGHT / info.rows);
  }
  if (width == 0)
    width = 1;
  if (height == 0)
    height = 1;
  uint32_t left = (LCD_WIDTH - width) / 2U;
  uint32_t top = (LCD_HEIGHT - height) / 2U;
  int top_down = (info.height_bits & 0x80000000U) != 0;
  memset(frame, 0, sizeof(frame));
  for (uint32_t y = 0; y < height; ++y)
  {
    uint32_t row = (uint32_t)((uint64_t)y * info.rows / height);  /* from the top */
    if (!top_down)
      row = info.rows - 1U - row;  /* a positive height stores rows bottom-up */
    if (!draw_row(flash, &info, info.pixel_offset + row * info.input_stride,
                  &frame[top + y][left], width))
    {
      DebugLog_Write("LCD: Flash read failed");
      return 0;
    }
  }
  DebugLog_Printf("LCD: %lux%lu BMP as %lux%lu gray, %lu ms",
                  (unsigned long)info.width, (unsigned long)info.rows,
                  (unsigned long)width, (unsigned long)height,
                  (unsigned long)(HAL_GetTick() - start));
  return 1;
}

/* Compared with the level read at start-up, so the button's polarity
   does not matter. */
void LcdTest_Poll(QSPI_HandleTypeDef *flash)
{
  if (!lcd_ready)
    return;
  GPIO_PinState now = HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_11);
  if (now != button_last && now != button_idle)
    LcdTest_ShowFlashBmp(flash);
  button_last = now;
}
