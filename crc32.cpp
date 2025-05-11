#include <nmmintrin.h>  // for _mm_crc32_u64
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

uint32_t crc32_hw(const void* data, size_t length) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t crc = 0xFFFFFFFF;

  while (length >= 8) 
  {
    uint64_t val;
    __builtin_memcpy(&val, p, sizeof(val));
    crc = _mm_crc32_u64(crc, val);
    p += 8;
    length -= 8;
  }

  while (length--) 
  {
    crc = _mm_crc32_u8(crc, *p++);
  }

  return ~crc;
}


uint8_t buffer[8 * 1024 * 1024];
uint32_t crc32(FILE *fp) {
  uint32_t crc = 0x00000000ULL;
  size_t read;

  read = fread(buffer, 1, sizeof(buffer), fp);
  crc = crc32_hw(buffer, read);
  return crc;
}
