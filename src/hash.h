#ifndef __STORFS_HASH_H__
#define __STORFS_HASH_H__

#include "storfs.h"

#include <stdint.h>

typedef struct {
  uint32_t page;
  uint32_t entry_count;
} Bucket;

typedef struct {
  uint32_t magic;
  uint32_t total_entries;
  uint32_t max_entries;
  uint32_t crc;
  Bucket   buckets[];
} Hash;

storfs_err_t hash_create(storfs_t *fs, storfs_page_t page);
storfs_err_t
hash_lookup(storfs_t *fs, storfs_page_t page, const char *name, Bucket *bucket);
storfs_err_t
hash_add(storfs_t *fs, storfs_page_t page, const char *name, Bucket bucket);

#endif
