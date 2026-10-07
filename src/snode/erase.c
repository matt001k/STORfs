#include "atomic.h"
#include "bitmap.h"
#include "common.h"
#include "snode.h"
#include "storfs.h"

#include <string.h>

static inline void decrement_extent(storfs_t     *fs,
                                    SNodeExtent  *extent,
                                    storfs_page_t pages_erased) {
  extent->count -= pages_erased;
  if(!extent->count) {
    extent->start = 0;
  }
}

static storfs_err_t
erase_snode_indirect_page(storfs_t *fs, SNodeInst *inst, storfs_page_t *page) {
  storfs_page_t page_init = *page;

  *page            = 0;
  storfs_err_t err = snode_update(fs, &inst->node, inst->page);
  if(err != STORFS_OK) {
    return err;
  }
  return bitmap_alloc_page(fs, page_init, PAGE_FREE);
}

static storfs_err_t erase_direct_extent(storfs_t        *fs,
                                        SNodeInst       *inst,
                                        SNodeOpInst     *op,
                                        SNodeExtentCache cache,
                                        void            *arg) {
  SNode        *node          = &inst->node;
  SNodeExtent  *direct_extent = &node->direct[cache.idx];
  storfs_page_t pages_erased  = *(storfs_page_t *)arg;

  decrement_extent(fs, direct_extent, pages_erased);
  return snode_update(fs, node, inst->page);
}

static storfs_err_t erase_indirect(storfs_t     *fs,
                                   SNodeExtent  *extent,
                                   storfs_size_t idx,
                                   storfs_page_t pages_erased,
                                   storfs_page_t indirect_page) {
  storfs_err_t err = atomic_read(fs, indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *tmp_extent = &((SNodeExtent *)fs->working_buf)[idx];
  decrement_extent(fs, tmp_extent, pages_erased);
  *extent = *tmp_extent;

  err = atomic_write(fs, indirect_page);
  if(err != STORFS_OK) {
    return err;
  }

  return err;
}

static storfs_err_t erase_indirect_extent(storfs_t        *fs,
                                          SNodeInst       *inst,
                                          SNodeOpInst     *op,
                                          SNodeExtentCache cache,
                                          void            *arg) {
  SNode        *node         = &inst->node;
  uint32_t      idx          = calc_single_idx(cache.idx);
  storfs_page_t pages_erased = *(storfs_page_t *)arg;
  SNodeExtent   extent;

  storfs_err_t err =
      erase_indirect(fs, &extent, idx, pages_erased, node->indirect.single);
  if(err != STORFS_OK) {
    return err;
  }

  if(!extent.start && !idx) {
    err = erase_snode_indirect_page(fs, inst, &node->indirect.single);
  }

  return err;
}

static storfs_err_t erase_multiple_extent(storfs_t        *fs,
                                          SNodeInst       *inst,
                                          SNodeOpInst     *op,
                                          SNodeExtentCache cache,
                                          void            *arg) {
  SNode           *node         = &inst->node;
  storfs_page_t    pages_erased = *(storfs_page_t *)arg;
  SNodeMultipleIdx idx          = calc_multiple_idx(fs, cache.idx);

  storfs_err_t err = atomic_read(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }
  SNodeMultiple *multiple = &((SNodeMultiple *)fs->working_buf)[idx.multiple];
  storfs_page_t  single_location = multiple->single_location;
  SNodeExtent    extent;
  err =
      erase_indirect(fs, &extent, idx.indirect, pages_erased, single_location);
  if(err != STORFS_OK) {
    return err;
  }

  // Safe to free single-indirect page now that parent is on flash
  if(!extent.start && !idx.indirect) {
    err = bitmap_alloc_page(fs, single_location, PAGE_FREE);
    if(err != STORFS_OK) {
      return err;
    }
  }

  err = atomic_read(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }
  multiple = &((SNodeMultiple *)fs->working_buf)[idx.multiple];
  multiple->total -= pages_erased;
  if(!multiple->total) {
    multiple->single_location = 0;
  }
  err = atomic_write(fs, node->indirect.multiple);
  if(err != STORFS_OK) {
    return err;
  }
  // If the first multiple extent is empty, free it
  if(!idx.multiple && !multiple->single_location) {
    err = erase_snode_indirect_page(fs, inst, &node->indirect.multiple);
  }

  return err;
}

static inline storfs_size_t
calculate_bytes_erased(const storfs_t         *fs,
                       const SNodeOpInst      *op,
                       const SNodeExtentCache *cache,
                       storfs_size_t          *bytes_in_extent) {
  storfs_size_t bytes_remaining = op->bytes_remaining;
  storfs_size_t bytes_in_full_extent =
      !cache->idx ? INLINE_DATA_SIZE(fs) : op->extent.count * fs->pageSize;

  *bytes_in_extent =
      cache->offset_bytes ? cache->offset_bytes : bytes_in_full_extent;

  return bytes_remaining > *bytes_in_extent ? *bytes_in_extent
                                            : bytes_remaining;
}

static inline storfs_page_t
calculate_pages_erased(const storfs_t         *fs,
                       const SNodeExtentCache *cache,
                       storfs_size_t           bytes_erased,
                       storfs_size_t           bytes_in_extent) {
  // If SNode page do not free SNODE
  if(!cache->idx) {
    return 0;
  }

  bool          fully_drains_extent = bytes_erased == bytes_in_extent;
  storfs_page_t pages_before        = CEIL_DIV(bytes_in_extent, fs->pageSize);
  storfs_page_t pages_after =
      CEIL_DIV(bytes_in_extent - bytes_erased, fs->pageSize);

  return pages_before - pages_after;
}

static storfs_err_t erase_partial_page(storfs_t               *fs,
                                       const SNodeExtentCache *cache,
                                       const SNodeOpInst      *op,
                                       storfs_page_t          *page_start,
                                       storfs_page_t           pages_erased) {
  uint32_t erase_byte_offset = op->bytes_remaining % fs->pageSize;
  if(!erase_byte_offset || pages_erased >= op->extent.count) {
    return STORFS_OK;
  }

  // Decrement page to previous page in the extent
  (*page_start)--;
  storfs_err_t err = atomic_read(fs, *page_start);
  if(err != STORFS_OK) {
    return err;
  }

  // First page, account for SNode size
  uint32_t page_capacity = !cache->idx ? INLINE_DATA_SIZE(fs) : fs->pageSize;
  uint32_t page_offset   = page_capacity - erase_byte_offset;
  uint32_t trim_bytes    = MIN(op->bytes_remaining, erase_byte_offset);
  memset(&fs->working_buf[page_offset], 0, trim_bytes);
  return atomic_write(fs, *page_start);
}

static storfs_err_t erase_data(storfs_t    *fs,
                               SNodeInst   *inst,
                               SNodeOpInst *op,
                               uint8_t     *data,
                               uint32_t     size) {
  storfs_err_t      err   = STORFS_OK;
  SNodeExtentCache *cache = &inst->write;
  storfs_size_t     bytes_in_extent;
  storfs_size_t     bytes_erased =
      calculate_bytes_erased(fs, op, cache, &bytes_in_extent);
  storfs_page_t pages_erased =
      calculate_pages_erased(fs, cache, bytes_erased, bytes_in_extent);
  uint32_t page_start = op->extent.start + op->extent.count - pages_erased;

  if(pages_erased) {
    err = bitmap_free_contiguous(fs, page_start, &pages_erased, pages_erased);
    if(err != STORFS_OK) {
      return err;
    }
  }

  if(pages_erased) {
    const SNodeHandleExtentCbs cbs = {
      erase_direct_extent,
      erase_indirect_extent,
      erase_multiple_extent,
    };
    err = get_modify_extents(fs, inst, op, cbs, &pages_erased);
  }
  if(err == STORFS_OK) {
    err = erase_partial_page(fs, cache, op, &page_start, pages_erased);
  }

  if(err == STORFS_OK) {
    cache->processed_bytes -= bytes_erased;
    op->bytes_remaining -= bytes_erased;
  }

  if(op->bytes_remaining) {
    cache->idx          = cache->idx ? cache->idx - 1 : 0;
    cache->offset_bytes = 0;
  } else if(err == STORFS_OK) {
    cache->offset_bytes = bytes_in_extent - bytes_erased;
  }

  return err;
}

/*!
 @brief Erase data from an snode instance

 @details Must call @link snode_find_write_location @endlink before erasing
          data from an snode. This function will erase data from the end
          of the snode. The cache write location will be updated with each
          erase.

 @param fs pointer to the filesystem instance
 @param inst pointer to snode instance
 @param size size of data to erase from the snode, the number of bytes erased
             are written here

 @return STORFS_OK on success
         STORFS_ERR_NULL_POINTER if NULL pointers passed into arguments
         STORFS_ERR_READ_FAILED if reading from the filesystem fails
         STORFS_ERR_ERASE_FAILED if erasing from the filesystem fails
         STORFS_ERR_WRITE_FAILED if writing from the filesystem fails
 */
storfs_err_t snode_erase_data(storfs_t *fs, SNodeInst *inst, uint32_t *size) {
  if(!fs || !inst) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNodeIdxCount idx = calc_snode_idx(fs);

  // Do not erase past end of an snode
  if(*size > inst->node.size) {
    *size = inst->node.size;
  }

  if(inst->write.idx > idx.multiple) {
    inst->write.idx = idx.multiple;
  }

  SNodeOpInst op = {
    .op              = SNODE_ERASE,
    .bytes_remaining = *size,
    .cb              = erase_data,
  };

  return snode_perform_op(fs, inst, &op, NULL, size);
}
