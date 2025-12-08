#include "fake_flash.h"

#include <stdint.h>
#include <string.h>

#define MEMORY_SIZE 33550336

#define PAGE_SIZE 512

struct FlashCtx {
  uint8_t flash_sim[MEMORY_SIZE];
  uint8_t fail_flag;
};

static struct FlashCtx ctx = { 0 };

static storfs_err_t storfs_read(const storfs_t *fs,
                                storfs_page_t   page,
                                storfs_byte_t   byte,
                                uint8_t        *buf,
                                storfs_size_t   size) {
  if(ctx.fail_flag & (1 << READ)) {
    return STORFS_ERROR;
  }

  if(page * PAGE_SIZE + byte + size > MEMORY_SIZE) {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++) {
    buf[i] = ctx.flash_sim[(PAGE_SIZE * page) + i + byte];
  }

  return STORFS_OK;
}

static storfs_err_t storfs_write(const storfs_t *fs,
                                 storfs_page_t   page,
                                 storfs_byte_t   byte,
                                 uint8_t        *buf,
                                 storfs_size_t   size) {
  if(ctx.fail_flag & (1 << WRITE)) {
    return STORFS_ERROR;
  }

  if(page * PAGE_SIZE + byte + size > MEMORY_SIZE) {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++) {
    ctx.flash_sim[(PAGE_SIZE * page) + i + byte] = buf[i];
  }
  return STORFS_OK;
}

static storfs_err_t storfs_erase(const storfs_t *fs, storfs_page_t page) {
  if(ctx.fail_flag & (1 << ERASE)) {
    return STORFS_ERROR;
  }

  for(int i = 0; i < PAGE_SIZE; i++) {
    ctx.flash_sim[(PAGE_SIZE * page) + i] = 0xFF;
  }
  return STORFS_OK;
}

static storfs_err_t storfs_sync(const storfs_t *fs) {
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
                .buf          = buf,
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

void fake_storfs_fail_op(FlashOperation op, bool fail) {
  if(fail) {
    ctx.fail_flag |= 1 << op;
  } else {
    ctx.fail_flag &= ~(1 << op);
  }
}
