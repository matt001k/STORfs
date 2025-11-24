#include "fake_flash.h"
#include <stdint.h>

#define MEMORY_SIZE  33550336

#define PAGE_SIZE 512
static uint8_t flash_sim[MEMORY_SIZE];

static storfs_err_t storfs_read(const struct storfs *fs, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buf, storfs_size_t size)
{
  if((byte + size) > PAGE_SIZE)
  {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++)
  {
    buf[i] = flash_sim[(PAGE_SIZE * page) + i + byte];
  }

  return STORFS_OK;
}

static storfs_err_t storfs_write(const struct storfs *fs, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buf, storfs_size_t size)
{
  if((byte + size) > PAGE_SIZE)
  {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++)
  {
     flash_sim[(PAGE_SIZE * page) + i + byte] = buf[i];
  }
  return STORFS_OK;
}

static storfs_err_t storfs_erase(const struct storfs *fs, storfs_page_t page)
{
  for(int i = 0; i < PAGE_SIZE; i++)
  {
    flash_sim[(PAGE_SIZE * page) + i] = 0xFF;
  }
return STORFS_OK;
}

static storfs_err_t storfs_sync(const struct storfs *fs)
{
return STORFS_OK;
}

storfs *fake_storfs_init(void) {}
    static storfs fs = {
        .read = storfs_read,
        .write = storfs_write,
        .erase = storfs_erase,
        .sync = storfs_sync,
        .memInst = NULL,
        .firstPageLoc = 0,
        .firstByteLoc = 0,
        .pageSize = PAGE_SIZE,
        .pageCount = MEMORY_SIZE / PAGE_SIZE,
    }:

    return fs;
}

