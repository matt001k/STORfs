#include "snode.h"

#include "atomic.h"
#include "bitmap.h"
#include "common.h"
#include "crc.h"

#include <string.h>

#define INLINE_DATA_SIZE(f)       (f->pageSize - sizeof(SNode))
#define NUM_MULTIPLE_EXTENTS(f)   ((f->pageSize / sizeof(SNodeMultiple)))
#define EXTENTS_PER_PAGE(f)       ((f->pageSize / sizeof(SNodeExtent)))
#define CALC_CONTIGUOUS_MAX(f, o) CEIL_DIV(o->bytes_remaining, f->pageSize)

typedef enum {
  SNODE_READ,
  SNODE_WRITE,
  SNODE_ERASE,
} SNodeOp;

typedef struct {
  SNodeExtent extent;  // Allocated extent on write, filled from flash otherwise
  SNodeOp     op;
  uint32_t    bytes_remaining;
} SNodeOpInst;

typedef struct {
  storfs_page_t single_location;
  storfs_page_t total;
} SNodeMultiple;

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

/*!
 @brief Create an snode

 @details This function will allocate a page for a new snode and save it.
          The snode will be available again through the @link
          snode_lookup @endlink function. When the snode is saved, a crc
          is calculated just on the data structure.

 @param fs pointer to the filesystem instance
 @param name snode name
 @param page page which has been allocated to the snode
 @param size type of snode:
                SNODE_TYPE_FILE
                SNODE_TYPE_DIR

 @return storfs_err_t
 */
storfs_err_t snode_create(storfs_t      *fs,
                          const char    *name,
                          storfs_page_t *page,
                          uint8_t        type) {
  if(!fs || !name || !page) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node = { 0 };
  storfs_err_t err  = snode_alloc_new_page(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  strncpy((char *)node.name, name, STORFS_MAX_FILE_NAME);
  node.name[STORFS_MAX_FILE_NAME - 1] = '\0';

  node.type = type;
  return snode_update(fs, &node, *page);
}

/*!
 @brief Lookup an snode based on a page location

 @details Finds information about an snode. Will validate the crc matches what
          is expected in order to validate the contents of the snode.

 @param fs pointer to the filesystem instance
 @param page page to obtain snode information
 @param inst pointer to snode instance

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_CRC_MISMATCH if crc calculation fails
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
 */
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

  inst->page  = page;
  inst->write = (SNodeExtentCache){ 0 };
  inst->read  = (SNodeExtentCache){ 0 };

  return STORFS_OK;
}

static storfs_err_t find_page_in_extents(SNodeExtentCache  *cache,
                                         const SNodeExtent *extents,
                                         uint32_t           count,
                                         uint32_t          *logical_page) {
  // If extents[i] == {0}, the extent is empty
  for(uint32_t i = 0; i < count && extents[i].count > 0; i++) {
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

static storfs_err_t find_double_indirect_location(storfs_t         *fs,
                                                  SNodeInst        *inst,
                                                  SNodeExtentCache *cache,
                                                  storfs_page_t *logical_page) {
  if(!inst->node.indirect.multiple) {
    return STORFS_ERR_NOT_FOUND;
  }

  storfs_err_t err = atomic_read(fs, inst->node.indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeMultiple *multiple                 = (SNodeMultiple *)fs->working_buf;
  storfs_page_t  single_indirect_location = 0;

  // Determine which indirect extent page holds the desired logical location
  for(uint32_t i = 0; i < NUM_MULTIPLE_EXTENTS(fs); i++) {
    if(!multiple[i].total) {
      break;
    }

    if(*logical_page < multiple[i].total) {
      single_indirect_location = multiple[i].single_location;
      break;
    }

    cache->idx += EXTENTS_PER_PAGE(fs);
    *logical_page -= multiple[i].total;
  }

  if(!single_indirect_location) {
    return STORFS_ERR_NOT_FOUND;
  }

  err = find_extent_in_indirect_page(fs,
                                     cache,
                                     single_indirect_location,
                                     logical_page);
  // The location is at the end of the data
  if(err == STORFS_ERR_NOT_FOUND) {
    err = STORFS_ERR_NO_SPACE;
  }

  return err;
}

static void
find_update_cache(storfs_t *fs, SNodeExtentCache *cache, storfs_loc_t logical) {
  cache->offset_bytes = logical.pageLoc * fs->pageSize + logical.byteLoc;
  // Increment here to account for index 0 being the snode inline data
  cache->idx++;
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

  // Find the logical page offset, how many pages would all the data consume
  // in a single contiguous block
  const uint32_t snode_inline_data_size = INLINE_DATA_SIZE(fs);

  // The logical page location is decremented throughout these operations if
  // it is non-zero, this will be the total offset in bytes from the extent's
  // starting location
  storfs_loc_t logical;
  uint32_t     data_beyond_snode = offset - snode_inline_data_size;
  logical.pageLoc                = data_beyond_snode / fs->pageSize;
  logical.byteLoc                = data_beyond_snode % fs->pageSize;

  // Determine if location is in direct extents
  uint32_t     extent_count = ARRAY_SIZE(inst->node.direct);
  storfs_err_t err          = find_page_in_extents(cache,
                                          inst->node.direct,
                                          extent_count,
                                          &logical.pageLoc);
  if(err == STORFS_OK) {
    find_update_cache(fs, cache, logical);
    return STORFS_OK;
  }

  if(!inst->node.indirect.single) {
    find_update_cache(fs, cache, logical);
    return STORFS_ERR_NOT_FOUND;
  }
  err = find_extent_in_indirect_page(fs,
                                     cache,
                                     inst->node.indirect.single,
                                     &logical.pageLoc);
  if(err == STORFS_OK) {
    find_update_cache(fs, cache, logical);
    return STORFS_OK;
  } else if(err != STORFS_ERR_NOT_FOUND) {
    return err;
  }

  err = find_double_indirect_location(fs, inst, cache, &logical.pageLoc);
  if(err != STORFS_OK && err != STORFS_ERR_NO_SPACE) {
    return err;
  }

  find_update_cache(fs, cache, logical);
  return err;
}

/*!
 @brief Find the read location extent index based on an offset byte location

 @details Will find the location to begin reading an snode from. This must be
          invoked before an snode is initially read. Will update the read cache
          when STORFS_OK, STORFS_ERR_NOT_FOUND or STORFS_ERR_NO_SPACE is
          returned.

 @param fs pointer to the filesystem instance
 @param page page to obtain snode information
 @param inst pointer to snode instance
 @param offset

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_NOT_FOUND could not find the location offset
         STORFS_ERR_NO_SPACE there is no more data to read from the file
         STORFS_ERR_CRC_MISMATCH if crc calculation fails
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
 */
storfs_err_t
snode_find_read_location(storfs_t *fs, SNodeInst *inst, storfs_byte_t offset) {
  if(!fs || !inst) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_location(fs, inst, &inst->read, offset);
}

/*!
 @brief Find the write location extent index based

 @details Will find the location to begin writing to or erasing from an snode.
          This must be before an snode is initially written to or erased from.
          Will update the write cache when STORFS_OK, STORFS_ERR_NOT_FOUND or
          STORFS_ERR_NO_SPACE is returned.

 @param fs pointer to the filesystem instance
 @param page page to obtain snode information
 @param inst pointer to snode instance

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_NO_SPACE there is no more data to write to the file
         STORFS_ERR_NOT_FOUND could not find the location offset
         STORFS_ERR_CRC_MISMATCH if crc calculation fails
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
 */
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

static inline storfs_page_t calculate_freed(const storfs_t    *fs,
                                            const SNodeExtent *extent,
                                            const SNodeOpInst *op) {
  storfs_page_t pages_remaining = op->bytes_remaining / fs->pageSize;
  storfs_page_t freed =
      pages_remaining > extent->count ? extent->count : pages_remaining;
  return freed;
}

static inline void erase_decrement_extent(const storfs_t    *fs,
                                          SNodeExtent       *extent,
                                          const SNodeOpInst *op) {
  // Decrement extent count by the number of pages contiguously freed
  extent->count -= calculate_freed(fs, extent, op);
  if(!extent->count) {
    extent->start = 0;
  }
}

static storfs_err_t
erase_snode_indirect_page(storfs_t *fs, SNodeInst *inst, storfs_page_t page) {
  storfs_err_t err = snode_update(fs, &inst->node, inst->page);
  if(err != STORFS_OK) {
    return err;
  }
  return bitmap_alloc_page(fs, page, PAGE_FREE);
}

static storfs_err_t process_extent_pages(storfs_t    *fs,
                                         SNodeOpInst *op,
                                         uint32_t     indirect_idx,
                                         uint32_t    *indirect_page) {
  storfs_err_t err = STORFS_OK;

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
    case SNODE_READ:
      op->extent.start = indirect_extent->start;
      op->extent.count = indirect_extent->count;
      break;
    case SNODE_ERASE:
      op->extent.start = indirect_extent->start;
      op->extent.count = indirect_extent->count;
      erase_decrement_extent(fs, indirect_extent, op);
      break;
  }

  if(op->op != SNODE_READ) {
    err = atomic_write(fs, *indirect_page);
  }

  if(op->op == SNODE_ERASE && !indirect_idx && !indirect_extent->start) {
    // If this is the first indirect index and it is empty
    *indirect_page = 0;
  }

  return err;
}

static storfs_err_t process_direct_extents(storfs_t        *fs,
                                           SNodeInst       *inst,
                                           SNodeOpInst     *op,
                                           SNodeExtentCache cache) {
  SNode       *node          = &inst->node;
  SNodeExtent *extent        = &op->extent;
  SNodeExtent *direct_extent = &node->direct[cache.idx];

  storfs_err_t err = STORFS_OK;
  extent->start    = direct_extent->start;
  extent->count    = direct_extent->count;

  if(op->op == SNODE_WRITE) {
    uint32_t max = CALC_CONTIGUOUS_MAX(fs, op);
    err = bitmap_alloc_contiguous(fs, &extent->start, &extent->count, max);
    if(err != STORFS_OK) {
      return err;
    }

    direct_extent->start = extent->start;
    direct_extent->count = extent->count;
  } else if(op->op == SNODE_ERASE) {
    erase_decrement_extent(fs, direct_extent, op);
  }

  if(op->op != SNODE_READ) {
    err = snode_update(fs, node, inst->page);
  }

  return err;
}

static storfs_err_t process_indirect_extents(storfs_t        *fs,
                                             SNodeInst       *inst,
                                             SNodeOpInst     *op,
                                             SNodeExtentCache cache) {
  SNode       *node                       = &inst->node;
  uint32_t     single_indirect_extent_idx = cache.idx - DIRECT_EXTENT_SIZE;
  storfs_err_t err                        = STORFS_OK;

  if(op->op == SNODE_WRITE && !node->indirect.single) {
    err = snode_alloc_indirect_page(fs, inst, &node->indirect.single);
    if(err != STORFS_OK) {
      return err;
    }
  }

  if(!node->indirect.single) {
    return STORFS_ERR_NOT_FOUND;
  }

  storfs_page_t init_single_indirect = node->indirect.single;

  err = process_extent_pages(fs,
                             op,
                             single_indirect_extent_idx,
                             &node->indirect.single);
  if(err != STORFS_OK) {
    return err;
  }

  if(op->op == SNODE_ERASE && !node->indirect.single && init_single_indirect) {
    err = erase_snode_indirect_page(fs, inst, init_single_indirect);
  }

  return err;
}

static storfs_err_t process_multiple_extents(storfs_t        *fs,
                                             SNodeInst       *inst,
                                             SNodeOpInst     *op,
                                             SNodeExtentCache cache) {
  storfs_err_t   err     = STORFS_OK;
  SNode         *node    = &inst->node;
  const uint32_t epp     = EXTENTS_PER_PAGE(fs);
  const uint32_t si_size = DIRECT_EXTENT_SIZE + epp;

  if(op->op == SNODE_WRITE && !node->indirect.multiple) {
    err = snode_alloc_indirect_page(fs, inst, &node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }
  }

  if(!node->indirect.multiple) {
    return STORFS_ERR_NOT_FOUND;
  }

  uint32_t single_indirect_page_idx = (cache.idx - si_size) / epp;
  err                               = atomic_read(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeMultiple *multiple_extents = (SNodeMultiple *)fs->working_buf;
  storfs_page_t  single_indirect_page =
      multiple_extents[single_indirect_page_idx].single_location;
  if(op->op == SNODE_WRITE && !single_indirect_page) {
    err = snode_alloc_new_page(fs, &single_indirect_page);
    if(err != STORFS_OK) {
      return err;
    }

    // Must re-read indirect multiple as snode_alloc_new_page clobers buffer
    err = atomic_read(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }

    multiple_extents = (SNodeMultiple *)fs->working_buf;
    multiple_extents[single_indirect_page_idx].single_location =
        single_indirect_page;

    err = atomic_write(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }
  }

  uint32_t      single_indirect_extent_idx = (cache.idx - si_size) % epp;
  storfs_page_t init_single_indirect       = single_indirect_page;

  err = process_extent_pages(fs,
                             op,
                             single_indirect_extent_idx,
                             &single_indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  // Only update multiple if it is on a byte boundary
  bool is_boundary = cache.offset_bytes % fs->pageSize == 0;

  if(op->op != SNODE_READ && is_boundary) {
    err = atomic_read(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }

    multiple_extents        = (SNodeMultiple *)fs->working_buf;
    SNodeMultiple *multiple = &multiple_extents[single_indirect_page_idx];
    if(op->op == SNODE_WRITE) {
      multiple->total += op->extent.count;
    } else {
      multiple->total -= calculate_freed(fs, &op->extent, op);
      if(!multiple->total) {
        multiple->single_location = 0;
      }
    }

    err = atomic_write(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }

    // Safe to free single-indirect page now that parent is on flash
    if(op->op == SNODE_ERASE && !single_indirect_page && init_single_indirect) {
      err = bitmap_alloc_page(fs, init_single_indirect, PAGE_FREE);
      if(err != STORFS_OK) {
        return err;
      }
    }

    bool empty_first_extent =
        !single_indirect_page_idx && !multiple->single_location;
    if(op->op == SNODE_ERASE && empty_first_extent) {
      storfs_page_t init_multiple_indirect = node->indirect.multiple;
      node->indirect.multiple              = 0;
      err = erase_snode_indirect_page(fs, inst, init_multiple_indirect);
    }
  }

  return err;
}

static storfs_err_t
get_modify_extents(storfs_t *fs, SNodeInst *inst, SNodeOpInst *op) {
  SNodeExtentCache extent_cache;

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
  // Subtract 1 as 1 is inline page
  extent_cache.idx -= 1;

  if(extent_cache.idx < DIRECT_EXTENT_SIZE) {
    return process_direct_extents(fs, inst, op, extent_cache);
  } else if(extent_cache.idx < si_size) {
    return process_indirect_extents(fs, inst, op, extent_cache);
  } else if(extent_cache.idx < mi_size) {
    return process_multiple_extents(fs, inst, op, extent_cache);
  }

  return STORFS_ERR_NO_FREE_BLOCKS;
}

static storfs_err_t snode_read_or_write_data(storfs_t         *fs,
                                             SNodeOpInst      *op,
                                             uint8_t          *data,
                                             uint32_t          size,
                                             SNodeExtentCache *cache) {
  storfs_loc_t location;
  location.pageLoc = op->extent.start + cache->offset_bytes / fs->pageSize;
  location.byteLoc = cache->offset_bytes % fs->pageSize;

  storfs_err_t err = atomic_read(fs, location.pageLoc);
  if(err != STORFS_OK) {
    return err;
  }

  storfs_page_t pages_accessed = 0;
  storfs_page_t pages_accessed_total =
      op->extent.count - (location.pageLoc - op->extent.start);
  uint32_t bytes_to_process = 0;

  // Loop through extents performing necessary action
  while(pages_accessed < pages_accessed_total) {
    uint32_t page_size_left = fs->pageSize - location.byteLoc;
    bytes_to_process        = MIN(op->bytes_remaining, page_size_left);
    uint32_t data_offset    = size - op->bytes_remaining;

    switch(op->op) {
      case SNODE_WRITE:
        memcpy(&fs->working_buf[location.byteLoc],
               &data[data_offset],
               bytes_to_process);

        err = atomic_write(fs, location.pageLoc);
        break;
      case SNODE_READ:
        memcpy(&data[data_offset],
               &fs->working_buf[location.byteLoc],
               bytes_to_process);
        if(op->bytes_remaining > bytes_to_process &&
           pages_accessed < pages_accessed_total - 1) {
          err = atomic_read(fs, location.pageLoc + 1);
        }
        break;
    }
    if(err != STORFS_OK) {
      break;
    }

    location.byteLoc = 0;
    location.pageLoc++;
    pages_accessed++;
    op->bytes_remaining -= bytes_to_process;
  }

  // Update offset within contiguous block
  if(err != STORFS_OK || !op->bytes_remaining) {
    if(pages_accessed) {
      cache->offset_bytes = (pages_accessed - 1) * fs->pageSize;
    }
    cache->offset_bytes += bytes_to_process;
  } else {
    cache->idx++;
    cache->offset_bytes = 0;
  }

  return err;
}

static storfs_err_t erase_data(storfs_t         *fs,
                               SNodeInst        *inst,
                               SNodeOpInst      *op,
                               SNodeExtentCache *cache) {
  storfs_err_t  err          = STORFS_OK;
  storfs_page_t freed        = calculate_freed(fs, &op->extent, op);
  uint32_t      page_start   = op->extent.start + op->extent.count - freed;
  uint32_t      pages_erased = freed;
  uint32_t      erase_bytes_offset = op->bytes_remaining % fs->pageSize;

  if(pages_erased) {
    err = bitmap_free_contiguous(fs, page_start, &pages_erased, pages_erased);
  }

  uint32_t bytes_erased = pages_erased * fs->pageSize;
  op->bytes_remaining -= bytes_erased;

  // Is there a partial erase needed for this extent?
  if(err == STORFS_OK && erase_bytes_offset &&
     pages_erased < op->extent.count) {
    // Erase page previous to the contiguous start location
    page_start--;
    err = atomic_read(fs, page_start);
    if(err != STORFS_OK) {
      goto finish;
    }

    // If page is SNode inline data the start of data increases by sizeof(SNode)
    erase_bytes_offset += inst->page == page_start ? sizeof(SNode) : 0;

    uint32_t zero_size = fs->pageSize - erase_bytes_offset;
    memset(&fs->working_buf[erase_bytes_offset], 0, zero_size);
    err = atomic_write(fs, page_start);
    if(err != STORFS_OK) {
      goto finish;
    }
    bytes_erased += op->bytes_remaining;
    op->bytes_remaining = 0;
  }

finish:
  if(err != STORFS_OK || !op->bytes_remaining) {
    cache->offset_bytes = op->extent.count * fs->pageSize - bytes_erased;
  } else {
    cache->idx          = cache->idx ? cache->idx - 1 : 0;
    cache->offset_bytes = 0;
  }

  return err;
}

static storfs_err_t snode_perform_op(storfs_t    *fs,
                                     SNodeInst   *inst,
                                     SNodeOpInst *op,
                                     uint8_t     *data,
                                     uint32_t     size) {
  if(!fs || !inst || (!data && op->op != SNODE_ERASE)) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_err_t      err   = STORFS_OK;
  SNodeExtentCache *cache = &inst->write;

  if(op->op == SNODE_READ) {
    cache = &inst->read;
  }

  // Do not erase past end of an snode
  if(op->op == SNODE_ERASE && size > inst->node.size) {
    return STORFS_ERR_INVALID_PARAM;
  }

  while(err == STORFS_OK && op->bytes_remaining) {
    err = get_modify_extents(fs, inst, op);
    if(err != STORFS_OK) {
      break;
    }

    if(op->op != SNODE_ERASE) {
      err = snode_read_or_write_data(fs, op, data, size, cache);
    } else {
      err = erase_data(fs, inst, op, cache);
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

/*!
 @brief Write data to an snode instance

 @details Must call @link snode_find_write_location @endlink before writing to
          the snode. This function will append data to the end of the snode.
          The cache write location will be updated with each write.

 @param fs pointer to the filesystem instance
 @param inst pointer to snode instance
 @param data data to write to the snode
 @param size size of data to write to the snode

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
         STORFS_ERR_ERASE_FAILED if erasing from the filesystem fails
         STORFS_ERR_WRITE_FAILED if writing from the filesystem fails
 */
storfs_err_t snode_write_data(storfs_t      *fs,
                              SNodeInst     *inst,
                              const uint8_t *data,
                              uint32_t       size) {
  SNodeOpInst op = { .op = SNODE_WRITE, .bytes_remaining = size };

  return snode_perform_op(fs, inst, &op, (uint8_t *)data, size);
}

/*!
 @brief Read data from an snode instance

 @details Must call @link snode_find_read_location @endlink before reading from
          an snode. This function will begin reading from the offset indicated
          in @link snode_find_read_location @endlink.

 @param fs pointer to the filesystem instance
 @param inst pointer to snode instance
 @param data data to read from the snode
 @param size size of data to read from the snode

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
         STORFS_ERR_ERASE_FAILED if erasing from the filesystem fails
         STORFS_ERR_WRITE_FAILED if writing from the filesystem fails
 */
storfs_err_t
snode_read_data(storfs_t *fs, SNodeInst *inst, uint8_t *data, uint32_t size) {
  SNodeOpInst op = { .op = SNODE_READ, .bytes_remaining = size };

  return snode_perform_op(fs, inst, &op, (uint8_t *)data, size);
}

/*!
 @brief Erase data from an snode instance

 @details Must call @link snode_find_write_location @endlink before erasing
          data from an snode. This function will erase data from the end
          of the snode. The cache write location will be updated with each
          erase.

 @param fs pointer to the filesystem instance
 @param inst pointer to snode instance
 @param size size of data to erase from the snode

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
         STORFS_ERR_ERASE_FAILED if erasing from the filesystem fails
         STORFS_ERR_WRITE_FAILED if writing from the filesystem fails
 */
storfs_err_t snode_erase_data(storfs_t *fs, SNodeInst *inst, uint32_t size) {
  SNodeOpInst op = { .op = SNODE_ERASE, .bytes_remaining = size };

  return snode_perform_op(fs, inst, &op, NULL, size);
}
