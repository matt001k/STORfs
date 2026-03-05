#include "common.h"

#include <stdint.h>

void uint16_t_to_uint8_t(uint8_t *buf, uint16_t uint16Val, uint32_t *index) {
  buf[*(index) + 1] = (uint8_t)(uint16Val);
  buf[*(index)]     = (uint8_t)(uint16Val >> 8);

  *index = *index + 2;
}

void uint32_t_to_uint8_t(uint8_t *buf, uint32_t uint32Val, uint32_t *index) {
  buf[*(index) + 3] = (uint8_t)(uint32Val);
  buf[*(index) + 2] = (uint8_t)(uint32Val >> 8);
  buf[*(index) + 1] = (uint8_t)(uint32Val >> 16);
  buf[*(index)]     = (uint8_t)(uint32Val >> 24);

  *index = *index + 4;
}

void uint64_t_to_uint8_t(uint8_t *buf, uint64_t uint64Val, uint32_t *index) {
  buf[*(index) + 7] = (uint8_t)(uint64Val);
  buf[*(index) + 6] = (uint8_t)(uint64Val >> 8);
  buf[*(index) + 5] = (uint8_t)(uint64Val >> 16);
  buf[*(index) + 4] = (uint8_t)(uint64Val >> 24);
  buf[*(index) + 3] = (uint8_t)(uint64Val >> 32);
  buf[*(index) + 2] = (uint8_t)(uint64Val >> 40);
  buf[*(index) + 1] = (uint8_t)(uint64Val >> 48);
  buf[*(index)]     = (uint8_t)(uint64Val >> 56);

  *index = *index + 8;
}

uint16_t uint8_t_to_uint16_t(uint8_t *buf, uint32_t *index) {
  uint16_t result = 0;
  result |= (uint16_t)buf[*(index) + 1];
  result |= (uint16_t)buf[*(index)] << 8;

  *index = *index + 2;

  return result;
}

uint32_t uint8_t_to_uint32_t(uint8_t *buf, uint32_t *index) {
  uint32_t result = 0;

  result |= (uint32_t)buf[*(index) + 3];
  result |= (uint32_t)buf[*(index) + 2] << 8;
  result |= (uint32_t)buf[*(index) + 1] << 16;
  result |= (uint32_t)buf[*(index)] << 24;

  *index = *index + 4;

  return result;
}

uint64_t uint8_t_to_uint64_t(uint8_t *buf, uint32_t *index) {
  uint64_t result = 0;
  result |= (uint64_t)buf[*(index) + 7];
  result |= (uint64_t)buf[*(index) + 6] << 8;
  result |= (uint64_t)buf[*(index) + 5] << 16;
  result |= (uint64_t)buf[*(index) + 4] << 24;
  result |= (uint64_t)buf[*(index) + 3] << 32;
  result |= (uint64_t)buf[*(index) + 2] << 40;
  result |= (uint64_t)buf[*(index) + 1] << 48;
  result |= (uint64_t)buf[*(index)] << 56;

  *index = *index + 8;

  return result;
}
