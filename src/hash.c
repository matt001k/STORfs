#include "hash.h"

#include "crc.h"

#include <string.h>

#define HASH_MAGIC       0xDEADB0B0
#define FNV_PRIME        16777619U
#define FNV_OFFSET_BASIS 2166136261U

struct HashBucketInfo {
  Bucket *bucket;
  Hash   *hash;
};

static uint32_t fnv1a_32(const void *data, uint32_t len) {
  uint32_t             hash = FNV_OFFSET_BASIS;
  const unsigned char *p    = (const unsigned char *)data;

  for(uint32_t i = 0; i < len; ++i) {
    hash ^= p[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

static storfs_err_t hash_get_info(storfs_t              *fs,
                                  storfs_page_t          page,
                                  const char            *name,
                                  struct HashBucketInfo *info) {
  info->bucket = NULL;
  info->hash   = (Hash *)fs->working_buf;

  if(fs->read(fs, page, 0, info->hash, fs->pageSize) != STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }

  if(info->hash->magic != HASH_MAGIC) {
    return STORFS_ERR_NOT_FOUND;
  }

  uint32_t hash = fnv1a_32(name, strnlen(name, STORFS_MAX_FILE_NAME));
  uint32_t i    = hash % info->hash->max_entries;

  info->bucket = &info->hash->buckets[i];

  return STORFS_OK;
}

storfs_err_t hash_create(storfs_t *fs, storfs_page_t page) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  Hash *hash_table = fs->working_buf;
  memset(hash_table, 0, fs->pageSize);
  hash_table->magic       = HASH_MAGIC;
  hash_table->max_entries = (fs->pageSize - sizeof(Hash)) / sizeof(Bucket);
  hash_table->crc         = storfs_crc32((uint8_t *)hash_table, fs->pageSize);

  return fs->write(fs, page, 0, (uint8_t *)hash_table, fs->pageSize);
}

storfs_err_t hash_lookup(storfs_t     *fs,
                         storfs_page_t page,
                         const char   *name,
                         Bucket       *bucket) {
  if(!fs || !name || !bucket) {
    return STORFS_ERR_NULL_POINTER;
  }

  struct HashBucketInfo info = {};
  storfs_err_t          err  = hash_get_info(fs, page, name, &info);

  if(err != STORFS_OK) {
    return err;
  }

  uint32_t crc   = info.hash->crc;
  info.hash->crc = 0;
  info.hash->crc = storfs_crc32((uint8_t *)info.hash, fs->pageSize);

  if(crc != info.hash->crc) {
    return STORFS_ERR_CRC_MISMATCH;
  }

  *bucket = *info.bucket;

  return err;
}

storfs_err_t
hash_add(storfs_t *fs, storfs_page_t page, const char *name, Bucket bucket) {
  if(!fs || !name) {
    return STORFS_ERR_NULL_POINTER;
  }

  // TODO: these reads could be optimized a bit, see what is cached in buf
  struct HashBucketInfo info = {};
  storfs_err_t          err  = hash_get_info(fs, page, name, &info);

  if(err == STORFS_OK) {
    *info.bucket = bucket;
    info.hash->total_entries++;
    info.hash->crc = 0;
    info.hash->crc = storfs_crc32((uint8_t *)info.hash, fs->pageSize);
    // TODO: replace with write_read_validate function
    err =
        fs->write(fs, page, 0, (uint8_t *)info.hash, fs->pageSize) != STORFS_OK
            ? STORFS_ERR_WRITE_FAILED
            : err;
  }

  return err;
}
