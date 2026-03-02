#ifndef __HELPER_RANDOMIZER_H__
#define __HELPER_RANDOMIZER_H__

#include <stddef.h>
#include <stdint.h>

uint8_t *random_array(size_t size);
void     random_array_free(uint8_t *buf);

#endif
