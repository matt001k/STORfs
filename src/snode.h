#ifndef __STORFS_SNODE_H__
#define __STORFS_SNODE_H__

#include "storfs.h"

#include <stdbool.h>
#include <stdint.h>

#define SNODE_TOTAL_SIZE 128

#define SNODE_INFO_SIZE 64
#define SNODE_RESERVED_SIZE                                                    \
  (SNODE_TOTAL_SIZE - STORFS_MAX_FILE_NAME - SNODE_INFO_SIZE)

#define DIRECT_EXTENT_SIZE 4

// Type flags
#define SNODE_TYPE_MASK 0x000F
#define SNODE_TYPE_FILE 0x0001
#define SNODE_TYPE_DIR  0x0002

#define SNODE_CHECK_TYPE_FREE(snode) (snode->type == 0)
#define SNODE_CHECK_TYPE_FILE(snode) (snode->type & SNODE_TYPE_FILE)
#define SNODE_CHECK_TYPE_DIR(snode)  (snode->type & SNODE_TYPE_DIR)

typedef struct {
  storfs_page_t start;
  storfs_page_t count;
} SNodeExtent;

typedef struct {
  uint64_t      modified_time;
  uint64_t      size;
  storfs_page_t extent_idx;
  SNodeExtent   direct[DIRECT_EXTENT_SIZE];
  struct {
    storfs_page_t single;
    storfs_page_t multiple;
  } indirect;
  uint16_t crc;
  uint8_t  type;
  uint8_t  flags;
  uint8_t  reserved[SNODE_RESERVED_SIZE];
  uint8_t  name[STORFS_MAX_FILE_NAME];
} SNode;

typedef struct {
  uint32_t offset_bytes;
  uint32_t idx;
} SNodeExtentCache;

typedef struct {
  SNode            node;
  storfs_page_t    page;
  SNodeExtentCache read;
  SNodeExtentCache write;
} SNodeInst;

_Static_assert(sizeof(SNode) == SNODE_TOTAL_SIZE,
               "Snode structure is not equivalent to expected size");

storfs_err_t snode_create(storfs_t *fs, const char *name, storfs_page_t *page);
storfs_err_t snode_lookup(storfs_t *fs, storfs_page_t page, SNodeInst *inst);
storfs_err_t
snode_find_read_location(storfs_t *fs, SNodeInst *inst, storfs_byte_t offset);
storfs_err_t snode_find_write_location(storfs_t *fs, SNodeInst *inst);
storfs_err_t snode_write_data(storfs_t      *fs,
                              SNodeInst     *inst,
                              const uint8_t *data,
                              uint32_t       size);
storfs_err_t
snode_read_data(storfs_t *fs, SNodeInst *inst, uint8_t *data, uint32_t size);
storfs_err_t snode_erase_data(storfs_t *fs, SNodeInst *inst, uint32_t size);
#endif
