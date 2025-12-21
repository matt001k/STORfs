#include "atomic.h"

storfs_err_t atomic_write(storfs_t *fs, storfs_page_t page) {
  // TODO: replace with atomic operation
  if(fs->erase(fs, page) != STORFS_OK) {
    return STORFS_ERR_ERASE_FAILED;
  }

  if(fs->write(fs, page, 0, fs->buf, fs->pageSize) != STORFS_OK) {
    return STORFS_ERR_WRITE_FAILED;
  }

  return STORFS_OK;
}
