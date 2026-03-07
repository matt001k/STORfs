#include "snode.h"

#include "atomic.h"
#include "bitmap.h"
#include "core.h"
#include "crc.h"

#include <string.h>

#define INLINE_DATA_SIZE(f) (f->pageSize - sizeof(SNode))
#define EXTENTS_PER_PAGE(f) ((f->pageSize / sizeof(SNodeExtent)))
#define DOUBLE_INDIRECT_EXTENT_SIZE(f)                                         \
  (EXTENTS_PER_PAGE(f) * EXTENTS_PER_PAGE(f) + SINGLE_INDIRECT_EXTENT_SIZE(f))
#define CALC_CONTIGUOUS_MAX(f, o) CEIL_DIV(o->bytes_remaining, f->pageSize)

typedef enum {
  SNODE_READ,
  SNODE_WRITE,
  SNODE_ERASE,
} SNodeOp;

typedef struct {
  SNodeExtent extent;
  SNodeOp     op;
  uint32_t    bytes_remaining;
} SNodeOpInst;

static storfs_err_t snode_alloc_new_page(storfs_t *fs, storfs_page_t *page) {
  storfs_err_t err = bitmap_alloc(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  memset(fs->working_buf, 0, fs->pageSize);

  return atomic_write(fs, *page);
}
static storfs_err_t
snode_update(storfs_t *fs, SNode *node, storfs_page_t page) {
  // Read snode page to preserve inline data
  storfs_err_t err = atomic_read(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  node->crc = 0;
  node->crc = storfs_crc16((const uint8_t *)node, sizeof(SNode));

  SNode *write_node = (SNode *)fs->working_buf;
  memcpy(write_node, node, sizeof(SNode));

  return atomic_write(fs, page);
}

storfs_err_t snode_create(storfs_t *fs, const char *name, storfs_page_t *page) {
  if(!fs || !name || !page) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node = { 0 };
  storfs_err_t err  = bitmap_alloc(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  strncpy((char *)node.name, name, STORFS_MAX_FILE_NAME);
  return snode_update(fs, &node, *page);
}

storfs_err_t snode_lookup(storfs_t *fs, storfs_page_t page, SNodeInst *inst) {
  if(!fs || !inst) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode       *node = &inst->node;
  storfs_err_t err  = atomic_read(fs, page);
  if(err != STORFS_OK) {
    return err;
  }
  memcpy(node, fs->working_buf, sizeof(SNode));

  uint32_t crc = node->crc;
  node->crc    = 0;
  node->crc    = storfs_crc16((uint8_t *)node, sizeof(SNode));

  if(crc != node->crc) {
    return STORFS_ERR_CRC_MISMATCH;
  }

  inst->page = page;

  return STORFS_OK;
}

static storfs_err_t find_page_in_extents(SNodeExtentCache  *cache,
                                         const SNodeExtent *extents,
                                         uint32_t           count,
                                         uint32_t          *logical_page) {
  uint32_t i = 0;

  // If extents[i] == 0, the extent is empty
  for(; i < count && extents[i].count > 0; i++) {
    if(*logical_page < extents[i].count) {
      return STORFS_OK;
    }
    cache->idx++;
    *logical_page -= extents[i].count;
  }

  return STORFS_ERR_NOT_FOUND;
}

static storfs_err_t find_extent_in_indirect_page(storfs_t         *fs,
                                                 SNodeExtentCache *cache,
                                                 storfs_page_t  indirect_page,
                                                 storfs_page_t *logical_page) {
  storfs_err_t err = atomic_read(fs, indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *extents      = (SNodeExtent *)fs->working_buf;
  uint32_t     extent_count = EXTENTS_PER_PAGE(fs);

  return find_page_in_extents(cache, extents, extent_count, logical_page);
}

static inline storfs_err_t
find_double_indirect_location(storfs_t         *fs,
                              SNodeInst        *inst,
                              SNodeExtentCache *cache,
                              storfs_page_t    *logical_page) {
  if(!inst->node.indirect.multiple) {
    return STORFS_ERR_NOT_FOUND;
  }

  storfs_err_t err = atomic_read(fs, inst->node.indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent  *extents                  = (SNodeExtent *)fs->working_buf;
  storfs_page_t single_indirect_location = 0;

  // Determine which indirect extent page holds the desired logical location
  uint32_t i = 0;
  for(; i < EXTENTS_PER_PAGE(fs); i++) {
    if(!extents[i].count) {
      break;
    }

    if(*logical_page < extents[i].count) {
      single_indirect_location = extents[i].start;
      break;
    }

    cache->idx += EXTENTS_PER_PAGE(fs);
    *logical_page -= extents[i].count;
  }

  if(!single_indirect_location) {
    return STORFS_ERR_NOT_FOUND;
  }

  err = find_extent_in_indirect_page(fs,
                                     cache,
                                     single_indirect_location,
                                     logical_page);
  return err;
}

static storfs_err_t find_location(storfs_t         *fs,
                                  SNodeInst        *inst,
                                  SNodeExtentCache *cache,
                                  storfs_byte_t     offset) {
  cache->idx = 0;

  if(offset < INLINE_DATA_SIZE(fs)) {
    cache->offset_bytes = sizeof(SNode) + offset;
    return STORFS_OK;
  }

  // Find the logical page offset, how many pages would the data consume
  // in a single contiguous block
  const uint32_t snode_inline_data_size = INLINE_DATA_SIZE(fs);
  storfs_loc_t   logical;
  uint32_t       data_beyond_snode = offset - snode_inline_data_size;
  logical.pageLoc                  = data_beyond_snode / fs->pageSize;
  logical.byteLoc                  = data_beyond_snode % fs->pageSize;

  storfs_err_t err          = STORFS_ERR_NOT_FOUND;
  uint32_t     extent_count = ARRAY_SIZE(inst->node.direct);
  err                       = find_page_in_extents(cache,
                             inst->node.direct,
                             extent_count,
                             &logical.pageLoc);
  if(err == STORFS_OK) {
    goto finish;
  }

  if(!inst->node.indirect.single) {
    goto finish;
  }
  err = find_extent_in_indirect_page(fs,
                                     cache,
                                     inst->node.indirect.single,
                                     &logical.pageLoc);
  if(err == STORFS_OK) {
    goto finish;
  } else if(err != STORFS_ERR_NOT_FOUND) {
    return err;
  }

  err = find_double_indirect_location(fs, inst, cache, &logical.pageLoc);
  if(err != STORFS_OK && err != STORFS_ERR_NOT_FOUND) {
    return err;
  }

finish:
  // logical.pageLoc is decremented throughout these operations if non zero,
  // this will be the total offset in bytes from the extent start location
  cache->offset_bytes = logical.pageLoc * fs->pageSize + logical.byteLoc;
  // Increment here as index 0 is the inline data
  cache->idx++;

  return err;
}

storfs_err_t
snode_find_read_location(storfs_t *fs, SNodeInst *inst, storfs_byte_t offset) {
  if(!fs || !inst) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_location(fs, inst, &inst->read, offset);
}
storfs_err_t snode_find_write_location(storfs_t *fs, SNodeInst *inst) {
  if(!fs || !inst) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_location(fs, inst, &inst->write, inst->node.size);
}

static storfs_err_t
snode_alloc_indirect_page(storfs_t *fs, SNodeInst *inst, storfs_page_t *page) {
  storfs_err_t err = snode_alloc_new_page(fs, page);
  if(err != STORFS_OK) {
    return err;
  }
  return snode_update(fs, &inst->node, inst->page);
}

static inline void
erase_decrement_extent(storfs_t *fs, SNodeExtent *extent, SNodeOpInst *op) {
  // Decrement extent count by the number of pages contiguously freed
  storfs_page_t pages_remaining = op->bytes_remaining / fs->pageSize;
  storfs_page_t decrement_count =
      pages_remaining > extent->count ? extent->count : pages_remaining;

  extent->count -= decrement_count;
  if(!extent->count) {
    extent->start = 0;
  }
}

static storfs_err_t
snode_read_maybe_alloc_extent_page(storfs_t    *fs,
                                   SNodeOpInst *op,
                                   uint32_t     indirect_idx,
                                   uint32_t    *indirect_page) {
  storfs_err_t err;

  if(op->op == SNODE_WRITE) {
    uint32_t max = CALC_CONTIGUOUS_MAX(fs, op);
    err =
        bitmap_alloc_contiguous(fs, &op->extent.start, &op->extent.count, max);
    if(err != STORFS_OK) {
      return err;
    }
  }

  err = atomic_read(fs, *indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *indirect_extent =
      &((SNodeExtent *)fs->working_buf)[indirect_idx];

  switch(op->op) {
    case SNODE_WRITE:
      indirect_extent->start = op->extent.start;
      indirect_extent->count = op->extent.count;
      break;
    case SNODE_ERASE:
      erase_decrement_extent(fs, indirect_extent, op);
      // intentional fallthrough
    case SNODE_READ:
      op->extent.start = indirect_extent->start;
      op->extent.count = indirect_extent->count;
  }

  if(op->op != SNODE_READ) {
    err = atomic_write(fs, *indirect_page);
  }

  if(op->op == SNODE_ERASE && !indirect_idx && !indirect_extent->start) {
    err            = bitmap_alloc_page(fs, *indirect_page, PAGE_FREE);
    *indirect_page = 0;
  }

  return err;
}

static storfs_err_t snode_op(storfs_t *fs, SNodeInst *inst, SNodeOpInst *op) {
  SNodeExtentCache extent_cache;
  SNode           *node   = &inst->node;
  SNodeExtent     *extent = &op->extent;

  const uint32_t epp     = EXTENTS_PER_PAGE(fs);
  const uint32_t si_size = DIRECT_EXTENT_SIZE + epp;
  const uint32_t mi_size = si_size + epp * epp;

  switch(op->op) {
    case SNODE_WRITE:
    case SNODE_ERASE:
      extent_cache = inst->write;
      break;
    case SNODE_READ:
    default:
      extent_cache = inst->read;
      break;
  }

  // Inline page
  if(!extent_cache.idx) {
    op->extent.start = inst->page;
    op->extent.count = 1;
    return STORFS_OK;
  }
  extent_cache.idx -= 1;

  storfs_err_t err = STORFS_OK;
  if(extent_cache.idx < DIRECT_EXTENT_SIZE) {
    SNodeExtent *direct_extent = &node->direct[extent_cache.idx];
    extent->start              = direct_extent->start;
    extent->count              = direct_extent->count;

    if(op->op == SNODE_WRITE) {
      uint32_t max = CALC_CONTIGUOUS_MAX(fs, op);
      err = bitmap_alloc_contiguous(fs, &extent->start, &extent->count, max);
      if(err != STORFS_OK) {
        return err;
      }

      direct_extent->start = extent->start;
      direct_extent->count = extent->count;

      err = snode_update(fs, node, inst->page);
      if(err != STORFS_OK) {
        return err;
      }
    } else if(op->op == SNODE_ERASE) {
      erase_decrement_extent(fs, direct_extent, op);
    }

    if(op->op != SNODE_READ) {
      err = snode_update(fs, node, inst->page);
    }
    if(err != STORFS_OK) {
      return err;
    }
  } else if(extent_cache.idx < si_size) {
    uint32_t single_indirect_extent_idx = extent_cache.idx - DIRECT_EXTENT_SIZE;
    if(op->op == SNODE_WRITE && !node->indirect.single) {
      err = snode_alloc_indirect_page(fs, inst, &node->indirect.single);
      if(err != STORFS_OK) {
        return err;
      }
    }

    if(!node->indirect.single) {
      return STORFS_ERR_NOT_FOUND;
    }

    err = snode_read_maybe_alloc_extent_page(fs,
                                             op,
                                             single_indirect_extent_idx,
                                             &inst->node.indirect.single);

    if(err != STORFS_OK) {
      return err;
    }
  } else if(extent_cache.idx < mi_size) {
    if(op->op == SNODE_WRITE && !node->indirect.multiple) {
      err = snode_alloc_indirect_page(fs, inst, &node->indirect.multiple);
      if(err != STORFS_OK) {
        return err;
      }
    }

    if(!node->indirect.multiple) {
      return STORFS_ERR_NOT_FOUND;
    }

    uint32_t single_indirect_page_idx =
        (extent_cache.idx - si_size) / EXTENTS_PER_PAGE(fs);
    err = atomic_read(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }

    SNodeExtent  *multiple_extents = (SNodeExtent *)fs->working_buf;
    storfs_page_t single_indirect_page =
        multiple_extents[single_indirect_page_idx].start;
    if(op->op == SNODE_WRITE && !single_indirect_page) {
      err = snode_alloc_new_page(fs, &single_indirect_page);
      if(err != STORFS_OK) {
        return err;
      }

      err = atomic_read(fs, node->indirect.multiple);
      if(err != STORFS_OK) {
        return err;
      }

      multiple_extents = (SNodeExtent *)fs->working_buf;
      multiple_extents[single_indirect_page_idx].start = single_indirect_page;

      err = atomic_write(fs, node->indirect.multiple);
      if(err != STORFS_OK) {
        return err;
      }
    }

    uint32_t single_indirect_extent_idx =
        (extent_cache.idx - si_size) % EXTENTS_PER_PAGE(fs);

    err = snode_read_maybe_alloc_extent_page(fs,
                                             op,
                                             single_indirect_extent_idx,
                                             &single_indirect_page);
    if(err != STORFS_OK) {
      return err;
    }

    if(op->op != SNODE_READ) {
      err = atomic_read(fs, node->indirect.multiple);
      if(err != STORFS_OK) {
        return err;
      }

      multiple_extents = (SNodeExtent *)fs->working_buf;
      SNodeExtent *multiple_extent =
          &multiple_extents[single_indirect_page_idx];
      if(op->op == SNODE_WRITE) {
        multiple_extent->count += op->extent.count;
      } else {
        multiple_extent->count -= op->extent.count;
        if(!multiple_extent->count) {
          multiple_extent->start = 0;
        }
      }

      err = atomic_write(fs, node->indirect.multiple);
      if(err != STORFS_OK) {
        return err;
      }

      bool empty_first_extent =
          !single_indirect_page_idx && !multiple_extent->start;
      if(op->op == SNODE_ERASE && empty_first_extent) {
        err = bitmap_alloc_page(fs, node->indirect.multiple, PAGE_FREE);
        if(err != STORFS_OK) {
          return err;
        }
        node->indirect.multiple = 0;
      }
    }

  } else {
    return STORFS_ERR_NO_FREE_BLOCKS;
  }

  return err;
}

static storfs_err_t snode_read_or_write_data(storfs_t    *fs,
                                             SNodeInst   *inst,
                                             SNodeOpInst *op,
                                             uint8_t     *data,
                                             uint32_t     size) {
  if(!fs || !inst || !data) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_err_t      err   = STORFS_OK;
  SNodeExtentCache *cache = &inst->write;

  if(op->op == SNODE_READ) {
    cache = &inst->read;
  }

  while(err == STORFS_OK && op->bytes_remaining) {
    err = snode_op(fs, inst, op);
    if(err != STORFS_OK) {
      break;
    }

    storfs_loc_t location;
    location.pageLoc = op->extent.start + cache->offset_bytes / fs->pageSize;
    location.byteLoc = cache->offset_bytes % fs->pageSize;
    err              = atomic_read(fs, location.pageLoc);
    if(err != STORFS_OK) {
      return err;
    }

    storfs_page_t pages_written = 0;
    storfs_page_t pages_to_write =
        op->extent.count - (location.pageLoc - op->extent.start);
    uint32_t write_size;
    while(pages_written < pages_to_write) {
      uint32_t page_size_left = fs->pageSize - location.byteLoc;
      write_size              = MIN(op->bytes_remaining, page_size_left);
      uint32_t data_offset    = size - op->bytes_remaining;

      switch(op->op) {
        case SNODE_WRITE:
          memcpy(&fs->working_buf[location.byteLoc],
                 &data[data_offset],
                 write_size);

          err = atomic_write(fs, location.pageLoc);
          break;
        case SNODE_READ:
          memcpy(&data[data_offset],
                 &fs->working_buf[location.byteLoc],
                 write_size);
          if(op->bytes_remaining > write_size) {
            err = atomic_read(fs, location.pageLoc + 1);
          }
          break;
        case SNODE_ERASE:
          break;
      }
      if(err != STORFS_OK) {
        break;
      }

      location.byteLoc = 0;
      location.pageLoc++;
      pages_written++;
      op->bytes_remaining -= write_size;
    }

    // Update offset within contiguous block
    if(err != STORFS_OK || !op->bytes_remaining) {
      if(pages_written) {
        cache->offset_bytes = (pages_written - 1) * fs->pageSize;
      }
      cache->offset_bytes += write_size;
    } else {
      if(op->op == SNODE_ERASE) {
        cache->idx = cache->idx ? cache->idx - 1 : 0;
      } else {
        cache->idx++;
      }
      cache->offset_bytes = 0;
    }
  }

  uint32_t processed_bytes = size - op->bytes_remaining;
  if(!processed_bytes) {
    return err;
  }

  if(op->op == SNODE_WRITE) {
    inst->node.size += processed_bytes;
  } else if(op->op == SNODE_ERASE) {
    inst->node.size -= processed_bytes;
  }

  if(op->op != SNODE_READ) {
    storfs_err_t update_err = snode_update(fs, &inst->node, inst->page);
    if(err == STORFS_OK) {
      err = update_err;
    }
  }

  return err;
}

storfs_err_t snode_write_data(storfs_t      *fs,
                              SNodeInst     *inst,
                              const uint8_t *data,
                              uint32_t       size) {
  storfs_err_t err = STORFS_OK;
  SNodeOpInst  op  = { .op = SNODE_WRITE, .bytes_remaining = size };

  return snode_read_or_write_data(fs, inst, &op, (uint8_t *)data, size);
}

storfs_err_t
snode_read_data(storfs_t *fs, SNodeInst *inst, uint8_t *data, uint32_t size) {

  storfs_err_t err = STORFS_OK;
  SNodeOpInst  op  = { .op = SNODE_READ, .bytes_remaining = size };

  return snode_read_or_write_data(fs, inst, &op, (uint8_t *)data, size);
}

storfs_err_t snode_erase_data(storfs_t *fs, SNodeInst *inst, uint32_t size) {

  storfs_err_t err       = STORFS_OK;
  SNodeOpInst  op        = { .op = SNODE_ERASE, .bytes_remaining = size };
  uint8_t      delete_me = 0;

  return snode_read_or_write_data(fs, inst, &op, &delete_me, size);
}
