#include "bmp_gray.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static void units(void)
{
  const uint8_t pixels[] = {0,0,0, 255,255,255, 0,0,255, 0,255,0, 255,0,0};
  const uint8_t expected[] = {0,255,77,149,29};
  uint8_t output[15];
  BmpGray_Convert(pixels, 5, output, 8);
  assert(memcmp(output, expected, 5) == 0);
  BmpGray_Convert(pixels, 5, output, 24);
  for (unsigned i = 0; i < 5; ++i)
    for (unsigned j = 0; j < 3; ++j) assert(output[i * 3 + j] == expected[i]);
  const uint8_t crc_input[] = "123456789";
  assert((BmpGray_CrcUpdate(0xFFFFFFFFU, crc_input, 9) ^ 0xFFFFFFFFU) == 0xCBF43926U);

  BmpGrayInfo source = { .width=3, .rows=2, .height_bits=2, .output_stride=12,
                        .output_size=78, .header_size=54, .output_bpp=24 };
  uint8_t header[1078], valid[54];
  BmpGray_MakeHeader(&source, header); memcpy(valid, header, 54);
  BmpGrayInfo info;
  assert(BmpGray_Parse(header, 78, 8, &info));
  assert(info.input_stride == 12 && info.output_stride == 4 && info.output_size == 1086);
  put32(header + 22, (uint32_t)-2);
  assert(BmpGray_Parse(header, 78, 24, &info) && info.rows == 2);
  put32(header + 22, 0x80000000U);
  assert(!BmpGray_Parse(header, 78, 8, &info));
  memcpy(header, valid, 54); put32(header + 18, 0x7FFFFFFFU);
  assert(!BmpGray_Parse(header, 78, 8, &info));
  memcpy(header, valid, 54); put32(header + 10, 0xFFFFFFFFU);
  assert(!BmpGray_Parse(header, 78, 8, &info));
  memcpy(header, valid, 54); put32(header + 14, 0xFFFFFFFFU);
  assert(!BmpGray_Parse(header, 78, 8, &info));
  memcpy(header, valid, 54); header[30] = 1;
  assert(!BmpGray_Parse(header, 78, 8, &info));
  memcpy(header, valid, 54);
  assert(!BmpGray_Parse(header, 77, 8, &info));
  assert(!BmpGray_Parse(header, 78, 16, &info));
}

int main(int argc, char **argv)
{
  units();
  if (argc == 4)
  {
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    assert(input && output);
    assert(fseek(input, 0, SEEK_END) == 0);
    long size = ftell(input); rewind(input);
    uint8_t header[1078], rgb[3072], gray[3072], zeros[3] = {0};
    assert(fread(header, 1, 54, input) == 54);
    BmpGrayInfo info;
    assert(BmpGray_Parse(header, (uint32_t)size, (uint8_t)atoi(argv[3]), &info));
    uint32_t h = BmpGray_MakeHeader(&info, header);
    assert(fwrite(header, 1, h, output) == h);
    for (uint32_t row = 0; row < info.rows; ++row)
    {
      for (uint32_t x = 0; x < info.width; x += 1024)
      {
        uint32_t n = info.width - x;
        if (n > 1024) n = 1024;
        assert(fseek(input, (long)(info.pixel_offset + row * info.input_stride + x * 3U), SEEK_SET) == 0);
        assert(fread(rgb, 1, n * 3U, input) == n * 3U);
        BmpGray_Convert(rgb, n, gray, info.output_bpp);
        uint32_t count = n * (info.output_bpp / 8U);
        assert(fwrite(gray, 1, count, output) == count);
      }
      uint32_t padding = info.output_stride - info.width * (info.output_bpp / 8U);
      assert(fwrite(zeros, 1, padding, output) == padding);
    }
    fclose(input); assert(fclose(output) == 0);
  }
  puts("BMP core tests: PASS (colors, CRC, strides, orientation, malformed headers)");
  return 0;
}
