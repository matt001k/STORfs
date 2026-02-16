#include "fake_flash.h"

#include <stdint.h>
#include <string.h>

#define MEMORY_SIZE 33550336

#define PAGE_SIZE 512

static storfs_err_t storfs_read(const struct storfs *fs,
                                storfs_page_t        page,
                                storfs_byte_t        byte,
                                uint8_t             *buf,
                                storfs_size_t        size) {
  if((byte + size) > PAGE_SIZE) {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++) {
    buf[i] = flash_sim[(PAGE_SIZE * page) + i + byte];
  }

  return STORFS_OK;
}

static storfs_err_t storfs_write(const struct storfs *fs,
                                 storfs_page_t        page,
                                 storfs_byte_t        byte,
                                 uint8_t             *buf,
                                 storfs_size_t        size) {
  if((byte + size) > PAGE_SIZE) {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++) {
    flash_sim[(PAGE_SIZE * page) + i + byte] = buf[i];
  }
  return STORFS_OK;
}

static storfs_err_t storfs_erase(const struct storfs *fs, storfs_page_t page) {
  for(int i = 0; i < PAGE_SIZE; i++) {
    flash_sim[(PAGE_SIZE * page) + i] = 0xFF;
  }
  return STORFS_OK;
}

static storfs_err_t storfs_sync(const struct storfs *fs) {
  return STORFS_OK;
}

storfs_t *fake_storfs_init(void) {
  static uint8_t  buf[PAGE_SIZE] = { 0 };
  static storfs_t fs             = {
                .read         = storfs_read,
                .write        = storfs_write,
                .erase        = storfs_erase,
                .sync         = storfs_sync,
                .memInst      = NULL,
                .firstPageLoc = 0,
                .firstByteLoc = 0,
                .pageSize     = PAGE_SIZE,
                .pageCount    = MEMORY_SIZE / PAGE_SIZE,
                .working_buf  = buf,
  };

  memset(buf, 0, sizeof(buf));
  memset(&ctx.flash_sim, 0xFF, sizeof(ctx.flash_sim));
  return &fs;
}

storfs_page_t fake_storfs_get_page_count(void) {
  return MEMORY_SIZE / PAGE_SIZE;
}

storfs_byte_t fake_storfs_get_page_size(void) {
  return PAGE_SIZE;
}

void fake_storfs_fail_op(FlashOperation op, bool fail, uint32_t count) {
  if(fail) {
    ctx.fail_flag |= 1 << op;
    ctx.fail_count[op] = count;
  } else {
    ctx.fail_flag &= ~(1 << op);
    ctx.fail_tracker[op] = 0;
  }
}
