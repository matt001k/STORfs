#include "atomic.h"
#include "common.h"
#include "crc.h"
#include "snode.h"

#include <string.h>

#define NUM_MULTIPLE_EXTENTS(f) ((f->pageSize / sizeof(SNodeMultiple)))

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

static storfs_err_t
find_multiple_indirect_location(storfs_t         *fs,
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

  SNodeMultiple *multiple                 = (SNodeMultiple *)fs->working_buf;
  storfs_page_t  single_indirect_location = 0;

  // Determine which indirect extent page holds the desired logical location
  uint32_t multiple_count = NUM_MULTIPLE_EXTENTS(fs);
  for(uint32_t i = 0; i < multiple_count; i++) {
    bool at_boundary = (i + 1 >= multiple_count) || !multiple[i + 1].total;
    if(*logical_page < multiple[i].total || at_boundary) {
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

  // The location is at the end of the data known data if not found
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
  cache->idx             = 0;
  cache->processed_bytes = offset;

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

  err = find_multiple_indirect_location(fs, inst, cache, &logical.pageLoc);
  if(err != STORFS_OK && err != STORFS_ERR_NOT_FOUND &&
     err != STORFS_ERR_NO_SPACE) {
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
