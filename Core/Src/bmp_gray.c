#include "bmp_gray.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void put32(uint8_t *p, uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void put16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }

int BmpGray_Parse(const uint8_t header[54], uint32_t file_size,
                  uint8_t output_bpp, BmpGrayInfo *info)
{
  if (file_size < 54 || header[0] != 'B' || header[1] != 'M' ||
      get32(header + 2) != file_size || (output_bpp != 8 && output_bpp != 24))
    return 0;
  uint32_t dib = get32(header + 14);
  uint32_t offset = get32(header + 10);
  uint32_t width = get32(header + 18);
  uint32_t height = get32(header + 22);
  if (dib < 40 || dib > file_size - 14 || offset < 14 + dib || offset > file_size ||
      width == 0 || width > 0x7FFFFFFFU || height == 0 || height == 0x80000000U ||
      get16(header + 26) != 1 || get16(header + 28) != 24 || get32(header + 30) != 0)
    return 0;
  uint32_t rows = (height & 0x80000000U) ? (~height + 1U) : height;
  uint64_t in_stride = ((uint64_t)width * 3U + 3U) & ~(uint64_t)3U;
  uint64_t out_stride = ((uint64_t)width * (output_bpp / 8U) + 3U) & ~(uint64_t)3U;
  uint32_t header_size = output_bpp == 8 ? 1078U : 54U;
  uint64_t output_size = header_size + out_stride * rows;
  if (in_stride * rows > file_size - offset || output_size > 0xFFFFFFFFU)
    return 0;
  info->width = width;
  info->rows = rows;
  info->height_bits = height;
  info->pixel_offset = offset;
  info->input_stride = (uint32_t)in_stride;
  info->output_stride = (uint32_t)out_stride;
  info->output_size = (uint32_t)output_size;
  info->header_size = header_size;
  info->output_bpp = output_bpp;
  info->x_ppm = get32(header + 38);
  info->y_ppm = get32(header + 42);
  return 1;
}

uint32_t BmpGray_MakeHeader(const BmpGrayInfo *info, uint8_t output[1078])
{
  memset(output, 0, info->header_size);
  output[0] = 'B'; output[1] = 'M';
  put32(output + 2, info->output_size);
  put32(output + 10, info->header_size);
  put32(output + 14, 40);
  put32(output + 18, info->width);
  put32(output + 22, info->height_bits);
  put16(output + 26, 1);
  put16(output + 28, info->output_bpp);
  put32(output + 34, info->output_stride * info->rows);
  put32(output + 38, info->x_ppm);
  put32(output + 42, info->y_ppm);
  if (info->output_bpp == 8)
  {
    put32(output + 46, 256);
    for (uint32_t i = 0; i < 256; ++i)
      output[54 + i * 4] = output[55 + i * 4] = output[56 + i * 4] = (uint8_t)i;
  }
  return info->header_size;
}

void BmpGray_Convert(const uint8_t *bgr, uint32_t pixels, uint8_t *output, uint8_t bpp)
{
  for (uint32_t i = 0; i < pixels; ++i)
  {
    uint8_t gray = (uint8_t)((29U * bgr[i * 3] + 150U * bgr[i * 3 + 1] +
                              77U * bgr[i * 3 + 2] + 128U) >> 8);
    if (bpp == 8) output[i] = gray;
    else output[i * 3] = output[i * 3 + 1] = output[i * 3 + 2] = gray;
  }
}

uint32_t BmpGray_CrcUpdate(uint32_t crc, const uint8_t *data, uint32_t count)
{
  for (uint32_t i = 0; i < count; ++i)
  {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
  }
  return crc;
}
