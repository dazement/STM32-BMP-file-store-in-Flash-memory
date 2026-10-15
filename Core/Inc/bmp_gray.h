#ifndef BMP_GRAY_H
#define BMP_GRAY_H
#include <stdint.h>

typedef struct {
  uint32_t width, rows, height_bits;
  uint32_t pixel_offset, input_stride, output_stride;
  uint32_t output_size, header_size, x_ppm, y_ppm;
  uint8_t output_bpp;
} BmpGrayInfo;

int BmpGray_Parse(const uint8_t header[54], uint32_t file_size,
                  uint8_t output_bpp, BmpGrayInfo *info);
uint32_t BmpGray_MakeHeader(const BmpGrayInfo *info, uint8_t output[1078]);
void BmpGray_Convert(const uint8_t *bgr, uint32_t pixels, uint8_t *output, uint8_t bpp);
uint32_t BmpGray_CrcUpdate(uint32_t crc, const uint8_t *data, uint32_t count);
#endif
