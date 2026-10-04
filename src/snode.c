#include "snode.h"

#include "atomic.h"
#include "bitmap.h"
#include "common.h"
#include "crc.h"

#include <string.h>

#define CALC_CONTIGUOUS_MAX(f, o) CEIL_DIV(o->bytes_remaining, f->pageSize)

static storfs_err_t snode_alloc_new_page(storfs_t *fs, storfs_page_t *page) {
  storfs_err_t err = bitmap_alloc(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  memset(fs->working_buf, 0, fs->pageSize);

  return atomic_write(fs, *page);
}

storfs_err_t snode_update(storfs_t *fs, SNode *node, storfs_page_t page) {
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

static storfs_err_t
snode_alloc_indirect_page(storfs_t *fs, SNodeInst *inst, storfs_page_t *page) {
  storfs_err_t err = snode_alloc_new_page(fs, page);
  if(err != STORFS_OK) {
    return err;
  }
  return snode_update(fs, &inst->node, inst->page);
}

static storfs_err_t process_extent_pages(storfs_t        *fs,
                                         SNodeOpInst     *op,
                                         uint32_t         indirect_idx,
                                         uint32_t         indirect_page,
                                         SNodeExtentCache cache) {
  storfs_err_t err = atomic_read(fs, indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *indirect_extent =
      &((SNodeExtent *)fs->working_buf)[indirect_idx];
  op->extent.start = indirect_extent->start;
  op->extent.count = indirect_extent->count;

  return err;
}

static storfs_err_t process_direct_extents(storfs_t        *fs,
                                           SNodeInst       *inst,
                                           SNodeOpInst     *op,
                                           SNodeExtentCache cache,
                                           void            *arg) {
  (void)arg;
  SNode       *node          = &inst->node;
  SNodeExtent *extent        = &op->extent;
  SNodeExtent *direct_extent = &node->direct[cache.idx];

  storfs_err_t err = STORFS_OK;
  extent->start    = direct_extent->start;
  extent->count    = direct_extent->count;

  return err;
}

static storfs_err_t process_indirect_extents(storfs_t        *fs,
                                             SNodeInst       *inst,
                                             SNodeOpInst     *op,
                                             SNodeExtentCache cache,
                                             void            *arg) {
  (void)arg;
  SNode       *node = &inst->node;
  uint32_t     idx  = calc_single_idx(cache.idx);
  storfs_err_t err  = STORFS_OK;

  if(!node->indirect.single) {
    return STORFS_OK;
  }

  return process_extent_pages(fs, op, idx, node->indirect.single, cache);
}

static storfs_err_t process_multiple_extents(storfs_t        *fs,
                                             SNodeInst       *inst,
                                             SNodeOpInst     *op,
                                             SNodeExtentCache cache,
                                             void            *arg) {
  (void)arg;
  SNode           *node = &inst->node;
  SNodeMultipleIdx idx  = calc_multiple_idx(fs, cache.idx);

  if(!node->indirect.multiple) {
    return STORFS_OK;
  }

  storfs_err_t err = atomic_read(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeMultiple *multiple_extents = (SNodeMultiple *)fs->working_buf;
  storfs_page_t  single_indirect_page =
      multiple_extents[idx.multiple].single_location;
  storfs_page_t init_single_indirect = single_indirect_page;

  return process_extent_pages(fs,
                              op,
                              idx.indirect,
                              single_indirect_page,
                              cache);
}

storfs_err_t get_modify_extents(storfs_t            *fs,
                                SNodeInst           *inst,
                                SNodeOpInst         *op,
                                SNodeHandleExtentCbs cbs,
                                void                *arg) {
  SNodeExtentCache extent_cache;
  SNodeIdxCount    idx = calc_snode_idx(fs);

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
    return cbs.direct(fs, inst, op, extent_cache, arg);
  } else if(extent_cache.idx < idx.single) {
    return cbs.single(fs, inst, op, extent_cache, arg);
  } else if(extent_cache.idx < idx.multiple) {
    return cbs.multiple(fs, inst, op, extent_cache, arg);
  }

  return STORFS_ERR_NO_FREE_BLOCKS;
}

static storfs_err_t create_direct_extent(storfs_t        *fs,
                                         SNodeInst       *inst,
                                         SNodeOpInst     *op,
                                         SNodeExtentCache cache,
                                         void            *arg) {
  SNode       *node          = &inst->node;
  SNodeExtent *extent        = &op->extent;
  SNodeExtent *direct_extent = &node->direct[cache.idx];
  uint32_t     max           = CALC_CONTIGUOUS_MAX(fs, op);
  storfs_err_t err =
      bitmap_alloc_contiguous(fs, &extent->start, &extent->count, max);
  if(err != STORFS_OK) {
    return err;
  }

  direct_extent->start = extent->start;
  direct_extent->count = extent->count;

  return snode_update(fs, node, inst->page);
}

static storfs_err_t create_indirect(storfs_t     *fs,
                                    SNodeOpInst  *op,
                                    SNodeExtent  *extent,
                                    storfs_size_t idx,
                                    storfs_page_t indirect_page) {
  uint32_t     max = CALC_CONTIGUOUS_MAX(fs, op);
  storfs_err_t err =
      bitmap_alloc_contiguous(fs, &extent->start, &extent->count, max);
  if(err != STORFS_OK) {
    return err;
  }

  err = atomic_read(fs, indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *indirect_extent = &((SNodeExtent *)fs->working_buf)[idx];
  indirect_extent->start       = extent->start;
  indirect_extent->count       = extent->count;

  return atomic_write(fs, indirect_page);
}

static storfs_err_t create_indirect_extent(storfs_t        *fs,
                                           SNodeInst       *inst,
                                           SNodeOpInst     *op,
                                           SNodeExtentCache cache,
                                           void            *arg) {
  SNode       *node   = &inst->node;
  SNodeExtent *extent = &op->extent;
  uint32_t     idx    = calc_single_idx(cache.idx);

  if(!node->indirect.single) {
    storfs_err_t err =
        snode_alloc_indirect_page(fs, inst, &node->indirect.single);
    if(err != STORFS_OK) {
      return err;
    }
  }

  return create_indirect(fs, op, extent, idx, node->indirect.single);
}
static storfs_err_t create_multiple_extent(storfs_t        *fs,
                                           SNodeInst       *inst,
                                           SNodeOpInst     *op,
                                           SNodeExtentCache cache,
                                           void            *arg) {
  SNode           *node   = &inst->node;
  SNodeExtent     *extent = &op->extent;
  SNodeMultipleIdx idx    = calc_multiple_idx(fs, cache.idx);
  storfs_err_t     err;

  if(!node->indirect.multiple) {
    err = snode_alloc_indirect_page(fs, inst, &node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }
  }

  err = atomic_read(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeMultiple *multiple_extents = (SNodeMultiple *)fs->working_buf;
  storfs_page_t  single_indirect_page =
      multiple_extents[idx.multiple].single_location;
  if(!single_indirect_page) {
    err = snode_alloc_new_page(fs, &single_indirect_page);
    if(err != STORFS_OK) {
      return err;
    }

    // Must re-read indirect multiple as snode_alloc_new_page clobbers buffer
    err = atomic_read(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }

    multiple_extents = (SNodeMultiple *)fs->working_buf;
    multiple_extents[idx.multiple].single_location = single_indirect_page;

    err = atomic_write(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }
  }

  err = create_indirect(fs, op, extent, idx.indirect, single_indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  // Multiple got clobbered again... re-read
  err = atomic_read(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }

  multiple_extents        = (SNodeMultiple *)fs->working_buf;
  SNodeMultiple *multiple = &multiple_extents[idx.multiple];
  multiple->total += op->extent.count;
  return atomic_write(fs, node->indirect.multiple);
}

static storfs_err_t snode_read_or_write_data(storfs_t    *fs,
                                             SNodeInst   *inst,
                                             SNodeOpInst *op,
                                             uint8_t     *data,
                                             uint32_t     size) {
  SNodeExtent      *extent = &op->extent;
  SNodeExtentCache *cache  = op->op == SNODE_WRITE ? &inst->write : &inst->read;
  storfs_err_t      err;

  // Only update the extent if it is on a byte boundary, else the current
  // block must be processed
  if(op->op == SNODE_WRITE && !cache->offset_bytes) {
    const SNodeHandleExtentCbs cbs = {
      create_direct_extent,
      create_indirect_extent,
      create_multiple_extent,
    };
    err = get_modify_extents(fs, inst, op, cbs, NULL);
    if(err != STORFS_OK) {
      return err;
    }
  }

  storfs_loc_t location;
  location.pageLoc = op->extent.start + cache->offset_bytes / fs->pageSize;
  location.byteLoc = cache->offset_bytes % fs->pageSize;

  err = atomic_read(fs, location.pageLoc);
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
      default:
        break;
    }

    op->bytes_remaining -= bytes_to_process;
    cache->processed_bytes += bytes_to_process;

    // Update offset within contiguous block. Must be computed before
    // location is advanced to the next page below, since it needs the
    // page/byte location the data was just written/read at.
    if(err != STORFS_OK || !op->bytes_remaining) {
      cache->offset_bytes =
          (location.pageLoc - op->extent.start) * fs->pageSize +
          location.byteLoc + bytes_to_process;
      break;
    }

    location.byteLoc = 0;
    location.pageLoc++;
    pages_accessed++;
  }

  if(op->bytes_remaining ||
     cache->offset_bytes == op->extent.count * fs->pageSize) {
    cache->idx++;
    cache->offset_bytes = 0;
  }

  SNodeIdxCount idx = calc_snode_idx(fs);
  if(cache->idx > idx.multiple) {
    err = STORFS_ERR_END_OF_FILE;
  }

  return err;
}

storfs_err_t snode_perform_op(storfs_t    *fs,
                              SNodeInst   *inst,
                              SNodeOpInst *op,
                              uint8_t     *data,
                              uint32_t    *size) {
  storfs_err_t err = STORFS_OK;

  const SNodeHandleExtentCbs cbs = {
    process_direct_extents,
    process_indirect_extents,
    process_multiple_extents,
  };
  while(err == STORFS_OK && op->bytes_remaining) {
    err = get_modify_extents(fs, inst, op, cbs, NULL);
    if(err != STORFS_OK) {
      break;
    }

    err = op->cb(fs, inst, op, data, *size);
  }

  // Set the number of bytes processed
  *size                    = *size - op->bytes_remaining;
  uint32_t processed_bytes = *size;

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
 @param size size of data to write to the snode, the total size is written
             here

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
         STORFS_ERR_ERASE_FAILED if erasing from the filesystem fails
         STORFS_ERR_WRITE_FAILED if writing from the filesystem fails
         STORFS_ERR_NO_FREE_BLOCKS if data is attempted to be writen beyond
                                   the bounds of the SNode
 */
storfs_err_t snode_write_data(storfs_t      *fs,
                              SNodeInst     *inst,
                              const uint8_t *data,
                              uint32_t      *size) {
  if(!fs || !inst || !data || !size) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNodeOpInst op = {
    .op              = SNODE_WRITE,
    .bytes_remaining = *size,
    .cb              = snode_read_or_write_data,
  };
  return snode_perform_op(fs, inst, &op, (uint8_t *)data, size);
}

/*!
 @brief Read data from an snode instance

 @details Must call @link snode_find_read_location @endlink before reading
 from an snode. This function will begin reading from the offset indicated in
 @link snode_find_read_location @endlink.

 @param fs pointer to the filesystem instance
 @param inst pointer to snode instance
 @param data data to read from the snode
 @param size size of data to read from the snode, the total size read is
             written here

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
         STORFS_ERR_ERASE_FAILED if erasing from the filesystem fails
         STORFS_ERR_WRITE_FAILED if writing from the filesystem fails
 */
storfs_err_t
snode_read_data(storfs_t *fs, SNodeInst *inst, uint8_t *data, uint32_t *size) {
  if(!fs || !inst || !data || !size) {
    return STORFS_ERR_NULL_POINTER;
  }

  // Ensures that reads cannot go beyond end of SNode
  storfs_err_t  storage_err = STORFS_OK;
  storfs_size_t remaining   = inst->node.size - inst->read.processed_bytes;
  if(*size > remaining) {
    storage_err = STORFS_ERR_END_OF_FILE;
    *size       = remaining;
  }

  SNodeOpInst op = {
    .op              = SNODE_READ,
    .bytes_remaining = *size,
    .cb              = snode_read_or_write_data,
  };
  storfs_err_t op_err = snode_perform_op(fs, inst, &op, (uint8_t *)data, size);
  if(op_err != STORFS_OK) {
    return op_err;
  }

  return storage_err;
}
