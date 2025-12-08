#include "helper_randomizer.h"

#include <stdlib.h>
#include <time.h>

uint8_t *random_array(size_t size) {
  uint8_t *buf;
  time_t   t = clock();
  srand(t * 10000);
  buf = (uint8_t *)malloc(size);
  for(size_t i = 0U; i < size; i++) {
    buf[i] = rand() % UINT8_MAX;
  }
  return buf;
}

void random_array_free(uint8_t *buf) {
  free(buf);
  buf = NULL;
}
