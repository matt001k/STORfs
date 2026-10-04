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

#define EXTENTS_PER_PAGE(f) ((f->pageSize / sizeof(SNodeExtent)))
#define INLINE_DATA_SIZE(f) (f->pageSize - sizeof(SNode))

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
  storfs_size_t processed_bytes;
  uint32_t      offset_bytes;
  uint32_t      idx;
} SNodeExtentCache;

typedef struct {
  SNode            node;
  storfs_page_t    page;
  SNodeExtentCache read;
  SNodeExtentCache write;
} SNodeInst;

_Static_assert(sizeof(SNode) == SNODE_TOTAL_SIZE,
               "Snode structure is not equivalent to expected size");

storfs_err_t
snode_create(storfs_t *fs, const char *name, storfs_page_t *page, uint8_t type);

storfs_err_t snode_lookup(storfs_t *fs, storfs_page_t page, SNodeInst *inst);
storfs_err_t
snode_find_read_location(storfs_t *fs, SNodeInst *inst, storfs_byte_t offset);
storfs_err_t snode_find_write_location(storfs_t *fs, SNodeInst *inst);

storfs_err_t snode_write_data(storfs_t      *fs,
                              SNodeInst     *inst,
                              const uint8_t *data,
                              uint32_t      *size);
storfs_err_t
snode_read_data(storfs_t *fs, SNodeInst *inst, uint8_t *data, uint32_t *size);
storfs_err_t snode_erase_data(storfs_t *fs, SNodeInst *inst, uint32_t *size);

typedef struct {
  storfs_page_t single_location;
  storfs_page_t total;
} SNodeMultiple;

typedef enum {
  SNODE_READ,
  SNODE_WRITE,
  SNODE_ERASE,
} SNodeOp;

struct SNodeOpInst;

typedef storfs_err_t (*SNodeOpCb)(storfs_t           *fs,
                                  SNodeInst          *inst,
                                  struct SNodeOpInst *op,
                                  uint8_t            *data,
                                  uint32_t            size);

typedef struct SNodeOpInst {
  SNodeExtent extent;  // Allocated extent on write, filled from flash otherwise
  SNodeOp     op;
  uint32_t    bytes_remaining;
  SNodeOpCb   cb;
} SNodeOpInst;

typedef struct {
  storfs_size_t single;
  storfs_size_t multiple;
} SNodeIdxCount;

typedef storfs_err_t (*ProcessExtentCb)(storfs_t        *fs,
                                        SNodeInst       *inst,
                                        SNodeOpInst     *op,
                                        SNodeExtentCache cache,
                                        void            *arg);

typedef struct {
  ProcessExtentCb direct;
  ProcessExtentCb single;
  ProcessExtentCb multiple;
} SNodeHandleExtentCbs;

typedef struct {
  storfs_size_t indirect;
  storfs_size_t multiple;
} SNodeMultipleIdx;

storfs_err_t snode_alloc_new_page(storfs_t *fs, storfs_page_t *page);
storfs_err_t snode_update(storfs_t *fs, SNode *node, storfs_page_t page);

storfs_err_t get_modify_extents(storfs_t            *fs,
                                SNodeInst           *inst,
                                SNodeOpInst         *op,
                                SNodeHandleExtentCbs cbs,
                                void                *arg);

storfs_err_t snode_perform_op(storfs_t    *fs,
                              SNodeInst   *inst,
                              SNodeOpInst *op,
                              uint8_t     *data,
                              uint32_t    *size);

static inline storfs_size_t calc_single_idx(storfs_size_t idx) {
  return idx - DIRECT_EXTENT_SIZE;
}

static inline SNodeIdxCount calc_snode_idx(const storfs_t *fs) {
  const uint32_t extents_per_page     = EXTENTS_PER_PAGE(fs);
  const uint32_t single_indirect_size = DIRECT_EXTENT_SIZE + extents_per_page;
  const uint32_t multiple_indirect_size =
      single_indirect_size + extents_per_page * extents_per_page;
  return (SNodeIdxCount){
    .single   = single_indirect_size,
    .multiple = multiple_indirect_size,
  };
}

static inline SNodeMultipleIdx calc_multiple_idx(const storfs_t *fs,
                                                 storfs_size_t   idx) {
  SNodeIdxCount  count = calc_snode_idx(fs);
  const uint32_t epp   = EXTENTS_PER_PAGE(fs);
  return (SNodeMultipleIdx){ (idx - count.single) % epp,
                             (idx - count.single) / epp };
}

#endif
