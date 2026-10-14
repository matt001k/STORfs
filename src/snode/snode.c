#include "snode.h"

#include "atomic.h"
#include "bitmap.h"
#include "crc.h"

#include <string.h>

/*!
 @brief Allocate a new page for an SNode operation

 @details Allocates a new page and then zeroes out the new page

 @param fs pointer to the filesystem instance
 @param page page which has been allocated to the snode

 @return storfs_err_t
 */
storfs_err_t snode_alloc_new_page(storfs_t *fs, storfs_page_t *page) {
  storfs_err_t err = bitmap_alloc(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  memset(fs->working_buf, 0, fs->pageSize);

  return atomic_write(fs, *page);
}

/*!
 @brief Update the SNode data structure

 @details Recalculates CRC16 and then saves the SNode to flash.

 @param fs pointer to the filesystem instance
 @param snode SNode to update
 @param page page where the SNode is located at

 @return storfs_err_t
 */
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
 @brief Create an SNode

 @details This function will allocate a page for a new snode and save it.
          The SNode will be available again through the @link
          snode_lookup @endlink function. Use that API to open the SNode in the
          future. When the snode is saved, a crc is calculated just on the data
          structure.

 @param fs pointer to the filesystem instance
 @param name snode name
 @param page page which has been allocated to the snode
 @param type type of snode:
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

  extent->start = direct_extent->start;
  extent->count = direct_extent->count;

  return STORFS_OK;
}

static storfs_err_t process_indirect_extents(storfs_t        *fs,
                                             SNodeInst       *inst,
                                             SNodeOpInst     *op,
                                             SNodeExtentCache cache,
                                             void            *arg) {
  (void)arg;
  SNode   *node = &inst->node;
  uint32_t idx  = calc_single_idx(cache.idx);

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

/*!
 @brief Function used to obtain or modify the extents in an SNode

 @details This is called with a number of callbacks used to get or modify:
            - Direct
            - Single Indirect
            - Multiple Indirect
          extents respectively. The three following functions:
            - process_direct_extents,
            - process_indirect_extents,
            - process_multiple_extents,
          are extremely helpful for obtaining the location in an extent
          to read from/access, they are used in @link snode_perform_op
          @endlink.

 @param fs pointer to the filesystem instance
 @param inst SNode instance to access
 @param op operation information to perform on the SNode
 @param cbs the callbacks associated with this functions invocation
 @param arg an argument to pass to the callbacks

 @return storfs_err_t
 */
storfs_err_t get_modify_extents(storfs_t            *fs,
                                SNodeInst           *inst,
                                SNodeOpInst         *op,
                                SNodeHandleExtentCbs cbs,
                                void                *arg) {
  SNodeExtentCache extent_cache = *op->cache;
  SNodeIdxCount    idx          = calc_snode_idx(fs);

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

/*!
 @brief Perform an operation on an SNode

 @details Find the location to access in the current SNode, based on the
          corresponding cache, then perform a specific operations withinA
          that location. Newly added SNode operations should leverage this
          function.

 @param fs pointer to the filesystem instance
 @param page page which has been allocated to the snode

 @return storfs_err_t
 */
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

  if(op->update_snode) {
    storfs_err_t update_err = snode_update(fs, &inst->node, inst->page);
    if(err == STORFS_OK) {
      err = update_err;
    }
  }

  return err;
}
