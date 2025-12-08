#ifndef __FAKE_FLASH_H__
#define __FAKE_FLASH_H__

#include "storfs.h"

#include <stdbool.h>

typedef enum {
  WRITE,
  ERASE,
  READ,
} FlashOperation;
storfs_t     *fake_storfs_init(void);
storfs_page_t fake_storfs_get_page_count(void);
storfs_byte_t fake_storfs_get_page_size(void);
void          fake_storfs_fail_op(FlashOperation op, bool fail);

#endif
