#ifndef __STORFS_BITMAP_H__
#define __STORFS_BITMAP_H__

#include "storfs.h"

#define PAGE_FREE  (0)
#define PAGE_ALLOC (1)

storfs_err_t bitmap_init(storfs_t *fs);
storfs_err_t bitmap_create(storfs_t *fs);
storfs_err_t bitmap_find_next(storfs_t *fs, storfs_page_t *page);
storfs_err_t bitmap_alloc(storfs_t *fs, storfs_page_t *page);
storfs_err_t bitmap_alloc_page(storfs_t *fs, storfs_page_t page, uint8_t alloc);
storfs_err_t bitmap_get_alloc(storfs_t *fs, storfs_page_t page, uint8_t *alloc);
storfs_err_t bitmap_get_contiguous(storfs_t      *fs,
                                   storfs_page_t *page,
                                   storfs_page_t *count,
                                   storfs_page_t  max);
storfs_err_t bitmap_alloc_contiguous(storfs_t      *fs,
                                     storfs_page_t *page,
                                     storfs_page_t *count,
                                     storfs_page_t  max);
storfs_err_t bitmap_free_contiguous(storfs_t      *fs,
                                    storfs_page_t  page,
                                    storfs_page_t *count,
                                    storfs_page_t  max);

#endif
