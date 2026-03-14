#ifndef __ATOMIC_H__
#define __ATOMIC_H__

#include "storfs.h"

storfs_err_t atomic_write(storfs_t *fs, storfs_page_t page);
storfs_err_t atomic_read(storfs_t *fs, storfs_page_t page);

#endif
