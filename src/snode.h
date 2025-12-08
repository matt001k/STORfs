#ifndef __STORFS_SNODE_H__
#define __STORFS_SNODE_H__

#include "storfs.h"

#include <stdint.h>

#define SNODE_TOTAL_SIZE 128

#define SNODE_INFO_SIZE 64
#define SNODE_RESERVED_SIZE                                                    \
  (SNODE_TOTAL_SIZE - STORFS_MAX_FILE_NAME - SNODE_INFO_SIZE)

#define DIRECT_BLOCKS_SIZE 8

// Type flags
#define SNODE_TYPE_MASK 0x000F
#define SNODE_TYPE_FILE 0x0001
#define SNODE_TYPE_DIR  0x0002

#define SNODE_CHECK_TYPE_FREE(snode) (snode->type == 0)
#define SNODE_CHECK_TYPE_FILE(snode) (snode->type & SNODE_TYPE_FILE)
#define SNODE_CHECK_TYPE_DIR(snode)  (snode->type & SNODE_TYPE_DIR)

typedef struct {
  uint32_t parent;
  struct {
    uint32_t page;
    uint32_t byte;
  } next;
  uint32_t size;
  uint32_t modified_time;
  uint32_t direct[DIRECT_BLOCKS_SIZE];
  struct {
    uint32_t single;
    uint32_t multiple;
  } indirect;
  uint16_t crc;
  uint8_t  type;
  uint8_t  flags;
  uint8_t  reserved[SNODE_RESERVED_SIZE];
  uint8_t  name[STORFS_MAX_FILE_NAME];
} SNode;

_Static_assert(sizeof(SNode) == SNODE_TOTAL_SIZE,
               "Snode structure is not equivalent to expected size");

storfs_err_t snode_create(storfs_t *fs, const char *name, storfs_page_t *page);
storfs_err_t snode_lookup(storfs_t *fs, storfs_page_t page, SNode *node);
storfs_err_t snode_write_data(storfs_t      *fs,
                              storfs_page_t  page,
                              const uint8_t *data,
                              uint32_t       size);
storfs_err_t snode_read_data(storfs_t     *fs,
                             storfs_page_t page,
                             storfs_byte_t offset,
                             uint8_t      *data,
                             uint32_t      size);
#endif
