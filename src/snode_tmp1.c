#include "atomic.h"
#include "bitmap.h"
#include "core.h"
#include "crc.h"
#include "snode.h"

#include <stdbool.h>
#include <string.h>

#define EXTENTS_PER_PAGE(f)    ((f->pageSize / sizeof(SNodeExtent)))
#define SINGLE_EXTENT_COUNT(f) (EXTENTS_PER_PAGE(f) + DIRECT_EXTENT_SIZE)
#define INLINE_DATA_SIZE(f)    (f->pageSize - sizeof(SNode))

typedef enum {
  WRITE,
  READ,
  ERASE,
} SNodeOp;

typedef struct {
  struct {
    storfs_page_t page;    // Snode page location
    storfs_byte_t offset;  // Snode page offset
    storfs_page_t max;     // Max pages to perform operations on
    SNodeOp       op;      // Operation being performed on SNode
  } request;
  struct {
    storfs_loc_t  location;    // Location of data
    storfs_page_t contiguous;  // Contiguous pages to perform operation on
  } result;
  struct {
    storfs_byte_t bytes;  // Bytes processed in the operation (SNodeOpCb)
    storfs_page_t pages;  // Pages processed in the operation (SNodeOpCb)
  } processed;
  struct {
    storfs_page_t extent_pages;  // Tracked number of extent pages
    storfs_byte_t
             page_op_size;  // Number of bytes necessary for a single operation
    uint32_t extent_idx;    // Track the number of extents when finding location
  } tracking;
} SNodeLocationInfo;

struct SNodeOpCtx;

typedef storfs_err_t (*SNodeOpCb)(storfs_t                *fs,
                                  SNodeLocationInfo       *info,
                                  SNode                   *node,
                                  const struct SNodeOpCtx *ctx);

typedef struct SNodeOpCtx {
  storfs_page_t page;    // SNode page location
  storfs_byte_t offset;  // Offset of SNode data
  SNodeOpCb     cb;      // Operation callback
  uint8_t      *data;    // Data point for SNode operation
  uint32_t      size;    // Size of data
  SNodeOp       op;      // Operation to perform on SNode
} SNodeOpCtx;

static inline storfs_err_t snode_check_read(storfs_t     *fs,
                                            storfs_page_t page,
                                            storfs_byte_t byte,
                                            uint8_t      *buf,
                                            uint32_t      size) {
  if(page >= fs->pageCount) {
    return STORFS_ERR_INVALID_PARAM;
  }

  storfs_err_t err = fs->read(fs, page, byte, buf, size);

  if(err != STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }

  return STORFS_OK;
}

static inline storfs_err_t alloc_page(storfs_t *fs, storfs_page_t *page) {
  storfs_err_t err;
  err = bitmap_alloc(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  memset(fs->working_buf, 0, fs->pageSize);

  return atomic_write(fs, *page);
}

static storfs_err_t ensure_node_page_allocated(storfs_t                *fs,
                                               storfs_page_t           *page,
                                               const SNodeLocationInfo *info) {
  storfs_err_t err;
  if(!*page && info->request.op == WRITE) {
    // Node structure will get saved at the end of write operation
    err = alloc_page(fs, page);
    if(err != STORFS_OK) {
      return err;
    }
  }

  return STORFS_OK;
}

static inline storfs_page_t calculate_contiguous(const SNodeLocationInfo *info,
                                                 storfs_loc_t logical,
                                                 SNodeExtent  extent) {

  storfs_page_t offset_in_extent =
      logical.pageLoc - info->tracking.extent_pages;
  storfs_page_t remaining_in_extent = extent.count - offset_in_extent;
  return MIN(info->request.max, remaining_in_extent);
}

static storfs_err_t find_page_in_extents(const SNodeExtent *extents,
                                         uint32_t          *count,
                                         uint32_t           logical_page,
                                         SNodeLocationInfo *info,
                                         storfs_page_t     *physical_page) {
  storfs_page_t *extent_offset = &info->tracking.extent_pages;
  storfs_page_t  i             = 0;

  for(; i < *count && extents[i].count > 0; i++) {
    if(logical_page < *extent_offset + extents[i].count) {
      *count         = i;
      *physical_page = extents[i].start + (logical_page - *extent_offset);
      info->tracking.extent_idx += i;
      return STORFS_OK;
    }
    *extent_offset += extents[i].count;
  }
  *count = i;
  return STORFS_ERR_NOT_FOUND;
}

static storfs_err_t find_extent_in_indirect_page(storfs_t     *fs,
                                                 storfs_page_t indirect_page,
                                                 SNodeLocationInfo *info,
                                                 storfs_loc_t       logical,
                                                 bool              *alloc) {
  storfs_page_t physical_page;
  storfs_err_t  err;

  err = snode_check_read(fs, indirect_page, 0, fs->working_buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *extents      = (SNodeExtent *)fs->working_buf;
  uint32_t     extent_count = EXTENTS_PER_PAGE(fs);

  return find_page_in_extents(extents,
                              &extent_count,
                              logical.pageLoc,
                              info,
                              &physical_page);
}

static storfs_err_t
find_inline_location(storfs_t *fs, SNode *node, SNodeLocationInfo *info) {
  const uint32_t snode_inline_data_size = INLINE_DATA_SIZE(fs);

  if(info->request.offset < snode_inline_data_size) {
    info->result.location.pageLoc = info->request.page;
    info->result.location.byteLoc = sizeof(SNode) + info->request.offset;
    info->result.contiguous       = 1;
    return STORFS_OK;
  }

  return STORFS_ERR_NOT_FOUND;
}

static inline storfs_err_t find_direct_location(storfs_t          *fs,
                                                SNode             *node,
                                                SNodeLocationInfo *info,
                                                storfs_loc_t       logical) {
  storfs_page_t physical_page;

  uint32_t extent_count = ARRAY_SIZE(node->direct);
  return find_page_in_extents(node->direct,
                              &extent_count,
                              logical.pageLoc,
                              info,
                              &physical_page);
}

static inline storfs_err_t
find_single_indirect_location(storfs_t          *fs,
                              SNode             *node,
                              SNodeLocationInfo *info,
                              storfs_loc_t      *logical) {
  // extent_idx is at least the size of the direct extents
  info->tracking.extent_idx = DIRECT_EXTENT_SIZE;

  if(!node->indirect.single) {
    return STORFS_ERR_NOT_FOUND;
  }
  return find_extent_in_indirect_page(fs,
                                      node->indirect.single,
                                      info,
                                      *logical,
                                      NULL);
}

static inline storfs_err_t
find_double_indirect_location(storfs_t          *fs,
                              SNode             *node,
                              SNodeLocationInfo *info,
                              storfs_loc_t      *logical) {
  // extent_idx is at least the size of the single indirect page
  info->tracking.extent_idx = SINGLE_EXTENT_COUNT(fs);

  if(!node->indirect.multiple) {
    return STORFS_ERR_NOT_FOUND;
  }

  storfs_err_t err = snode_check_read(fs,
                                      node->indirect.multiple,
                                      0,
                                      fs->working_buf,
                                      fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent   *extents                  = (SNodeExtent *)fs->working_buf;
  storfs_page_t *pages_seen               = &info->tracking.extent_pages;
  storfs_page_t  single_indirect_location = 0;
  bool           is_write                 = info->request.op == WRITE;

  uint32_t i = 0;
  // Determine which indirect extent page holds the desired logical location
  for(; i < EXTENTS_PER_PAGE(fs); i++) {
    if(!extents[i].count) {
      break;
    }

    if(logical->pageLoc < *pages_seen + extents[i].count) {
      single_indirect_location = extents[i].start;
      break;
    }

    *pages_seen += extents[i].count;
  }

  bool alloc = false;
  err        = find_extent_in_indirect_page(fs,
                                     single_indirect_location,
                                     info,
                                     *logical,
                                     &alloc);
  if(err != STORFS_OK) {
    return err;
  }

  info->tracking.extent_idx += i * EXTENTS_PER_PAGE(fs);

  return err;
}

static storfs_err_t
find_data_location(storfs_t *fs, SNode *node, SNodeLocationInfo *info) {

  if(find_inline_location(fs, node, info) == STORFS_OK) {
    return STORFS_OK;
  }

  storfs_err_t err;

  // Find the logical page offset, how many pages would the data consume
  // in a single contiguous block
  const uint32_t snode_inline_data_size = INLINE_DATA_SIZE(fs);
  storfs_loc_t   logical;
  uint32_t data_beyond_snode = info->request.offset - snode_inline_data_size;
  logical.pageLoc            = data_beyond_snode / fs->pageSize;
  logical.byteLoc            = data_beyond_snode % fs->pageSize;

  if(find_direct_location(fs, node, info, logical) == STORFS_OK) {
    return STORFS_OK;
  }

  err = find_single_indirect_location(fs, node, info, &logical);
  if(err != STORFS_ERR_NOT_FOUND) {
    return err;
  }

  return find_double_indirect_location(fs, node, info, &logical);
}

static storfs_err_t
get_location_info(storfs_t *fs, SNode *node, SNodeLocationInfo *info) {
  storfs_err_t err = find_data_location(fs, node, info);
  if(err != STORFS_OK) {
    return err;
  }

  // Skip reading the page for aligned erases
  if(info->request.op != ERASE || info->result.location.byteLoc) {
    err = snode_check_read(fs,
                           info->result.location.pageLoc,
                           0,
                           fs->working_buf,
                           fs->pageSize);
  }

  return err;
}

static storfs_err_t
snode_update(storfs_t *fs, SNode *node, storfs_page_t page) {
  storfs_err_t err =
      snode_check_read(fs, page, 0, fs->working_buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  node->crc = 0;
  node->crc = storfs_crc16((const uint8_t *)node, sizeof(SNode));

  SNode *write_node = (SNode *)fs->working_buf;
  *write_node       = *node;

  return atomic_write(fs, page);
}

static storfs_err_t snode_op(storfs_t *fs, const SNodeOpCtx *ctx) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node;
  storfs_err_t err  = snode_lookup(fs, ctx->page, &node);
  uint32_t     size = ctx->size;

  if(ctx->op == ERASE) {
    if(ctx->offset >= node.size) {
      return STORFS_ERR_INVALID_PARAM;
    }
    size = node.size - ctx->offset;
  }

  SNodeLocationInfo info = { 0 };
  while(err == STORFS_OK && info.processed.bytes < size) {
    if(ctx->op == WRITE) {
      info.request.offset = node.size;
    } else {
      info.request.offset = info.processed.bytes + ctx->offset;
    }
    info.request.page = ctx->page;
    info.request.op   = ctx->op;
    info.request.max  = CEIL_DIV(size - info.processed.bytes, fs->pageSize);
    info.tracking.extent_pages = 0;
    info.tracking.extent_idx   = 0;

    err = get_location_info(fs, &node, &info);
    if(err != STORFS_OK) {
      break;
    }

    info.processed.pages = 0;
    while(info.processed.pages < info.result.contiguous) {
      uint32_t page_size_left    = fs->pageSize - info.result.location.byteLoc;
      uint32_t bytes_remaining   = size - info.processed.bytes;
      info.tracking.page_op_size = MIN(bytes_remaining, page_size_left);

      err = ctx->cb(fs, &info, &node, ctx);
      if(err != STORFS_OK) {
        break;
      }
      info.processed.pages++;
    }
  }

  if((ctx->op == WRITE || ctx->op == ERASE) && info.processed.bytes) {
    // Update snode even on error
    storfs_err_t snode_update_err = snode_update(fs, &node, ctx->page);

    if(err == STORFS_OK) {
      err = snode_update_err;
    }
  }

  return err;
}

storfs_err_t snode_create(storfs_t *fs, const char *name, storfs_page_t *page) {
  if(!fs || !name || !page) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node = { 0 };
  storfs_err_t err  = bitmap_find_next(fs, page);

  if(err != STORFS_OK) {
    return err;
  }

  strncpy((char *)node.name, name, STORFS_MAX_FILE_NAME);
  err = snode_update(fs, &node, *page);
  if(err != STORFS_OK) {
    return err;
  }

  return bitmap_alloc_page(fs, *page, PAGE_ALLOC);
}

storfs_err_t snode_lookup(storfs_t *fs, storfs_page_t page, SNode *node) {
  if(!fs || !node) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_err_t err =
      snode_check_read(fs, page, 0, (uint8_t *)node, sizeof(SNode));
  if(err != STORFS_OK) {
    return err;
  }

  uint32_t crc = node->crc;
  node->crc    = 0;
  node->crc    = storfs_crc16((uint8_t *)node, sizeof(SNode));

  if(crc != node->crc) {
    return STORFS_ERR_CRC_MISMATCH;
  }

  return STORFS_OK;
}

static storfs_err_t snode_write_op(storfs_t          *fs,
                                   SNodeLocationInfo *info,
                                   SNode             *node,
                                   const SNodeOpCtx  *ctx) {
  // Zero buffer when starting at page boundary
  if(!info->result.location.byteLoc) {
    memset(fs->working_buf, 0, fs->pageSize);
  }
  memcpy(&fs->working_buf[info->result.location.byteLoc],
         &ctx->data[info->processed.bytes],
         info->tracking.page_op_size);

  storfs_err_t err = atomic_write(fs, info->result.location.pageLoc);
  if(err == STORFS_OK) {
    info->result.location.byteLoc = 0;
    info->result.location.pageLoc++;
    node->size += info->tracking.page_op_size;
    info->processed.bytes += info->tracking.page_op_size;
  }

  return err;
}

storfs_err_t snode_write_data(storfs_t      *fs,
                              storfs_page_t  page,
                              const uint8_t *data,
                              uint32_t       size) {
  SNodeOpCtx ctx = {
    .page   = page,
    .offset = 0,
    .cb     = snode_write_op,
    .data   = (uint8_t *)data,
    .size   = size,
    .op     = WRITE,
  };

  return snode_op(fs, &ctx);
}

static storfs_err_t snode_read_op(storfs_t          *fs,
                                  SNodeLocationInfo *info,
                                  SNode             *node,
                                  const SNodeOpCtx  *ctx) {
  (void)node;

  memcpy(&ctx->data[info->processed.bytes],
         &fs->working_buf[info->result.location.byteLoc],
         info->tracking.page_op_size);

  info->result.location.byteLoc = 0;
  info->result.location.pageLoc++;
  info->processed.bytes += info->tracking.page_op_size;

  // Not needed if operation is complete
  if(info->result.contiguous - info->processed.pages > 1) {
    return snode_check_read(fs,
                            info->result.location.pageLoc,
                            0,
                            fs->working_buf,
                            fs->pageSize);
  }

  return STORFS_OK;
}

storfs_err_t snode_read_data(storfs_t     *fs,
                             storfs_page_t page,
                             storfs_byte_t offset,
                             uint8_t      *data,
                             uint32_t      size) {
  SNodeOpCtx ctx = {
    .page   = page,
    .offset = offset,
    .cb     = snode_read_op,
    .data   = (uint8_t *)data,
    .size   = size,
    .op     = READ,
  };

  return snode_op(fs, &ctx);
}

static storfs_err_t free_indirect_page_if_empty(storfs_t      *fs,
                                                storfs_page_t *page) {
  SNodeExtent *extents = (SNodeExtent *)fs->working_buf;
  for(uint32_t i = 0; i < EXTENTS_PER_PAGE(fs); i++) {
    if(extents[i].count) {
      return STORFS_OK;
    }
  }

  storfs_err_t err = bitmap_alloc_page(fs, *page, PAGE_FREE);
  if(err == STORFS_OK) {
    *page = 0;
  }

  return err;
}

static inline void erase_decrement_extent(SNodeExtent             *extent,
                                          const SNodeLocationInfo *info) {
  // Decrement extent count by the number of pages contiguously freed
  extent->count -= info->result.contiguous;
  if(!extent->count) {
    extent->start = 0;
  }
}

static storfs_err_t erase_indirect_op(storfs_t                *fs,
                                      const SNodeLocationInfo *info,
                                      storfs_page_t           *page,
                                      uint32_t                 idx) {
  storfs_err_t err =
      snode_check_read(fs, *page, 0, fs->working_buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *extents = (SNodeExtent *)fs->working_buf;
  erase_decrement_extent(&extents[idx], info);

  err = atomic_write(fs, *page);
  if(err != STORFS_OK) {
    return err;
  }

  return free_indirect_page_if_empty(fs, page);
}

static storfs_err_t
erase_handle_extents(storfs_t *fs, SNodeLocationInfo *info, SNode *node) {
  uint32_t     idx = info->tracking.extent_idx;
  storfs_err_t err = STORFS_OK;

  if(idx < DIRECT_EXTENT_SIZE) {
    erase_decrement_extent(&node->direct[idx], info);
  } else if(idx < SINGLE_EXTENT_COUNT(fs)) {
    uint32_t single_idx = idx - DIRECT_EXTENT_SIZE;

    err = erase_indirect_op(fs, info, &node->indirect.single, single_idx);
    if(err != STORFS_OK) {
      return err;
    }

  } else {
    uint32_t total_idx    = idx - SINGLE_EXTENT_COUNT(fs);
    uint32_t multiple_idx = total_idx / EXTENTS_PER_PAGE(fs);
    uint32_t single_idx   = total_idx % EXTENTS_PER_PAGE(fs);

    err = snode_check_read(fs,
                           node->indirect.multiple,
                           0,
                           fs->working_buf,
                           fs->pageSize);
    if(err != STORFS_OK) {
      return err;
    }

    // Update indirect page
    SNodeExtent  *extents       = (SNodeExtent *)fs->working_buf;
    storfs_page_t indirect_page = extents[multiple_idx].start;
    err = erase_indirect_op(fs, info, &indirect_page, single_idx);
    if(err != STORFS_OK) {
      return err;
    }

    // Re-read multiple extent page, update it
    err = snode_check_read(fs,
                           node->indirect.multiple,
                           0,
                           fs->working_buf,
                           fs->pageSize);
    if(err != STORFS_OK) {
      return err;
    }

    extents                      = (SNodeExtent *)fs->working_buf;
    SNodeExtent *multiple_extent = &extents[multiple_idx];
    erase_decrement_extent(multiple_extent, info);
    err = atomic_write(fs, node->indirect.multiple);
    if(err != STORFS_OK) {
      return err;
    }

    // Free outer page if empty
    err = free_indirect_page_if_empty(fs, &node->indirect.multiple);
  }

  return err;
}

static storfs_err_t snode_erase_op(storfs_t          *fs,
                                   SNodeLocationInfo *info,
                                   SNode             *node,
                                   const SNodeOpCtx  *ctx) {
  storfs_err_t  err             = STORFS_OK;
  storfs_byte_t processed_bytes = 0;

  // Node size will be the truncated offset
  node->size = ctx->offset;

  // Only invoke once per iteration
  info->processed.pages = info->result.contiguous;

  // If the byte location is not zero offset, write partial page
  if(info->result.location.byteLoc) {
    memset(&fs->working_buf[info->result.location.byteLoc],
           0,
           info->tracking.page_op_size);
    err = atomic_write(fs, info->result.location.pageLoc);
    if(err != STORFS_OK) {
      return err;
    }
    processed_bytes = info->tracking.page_op_size;
    info->result.location.pageLoc++;
    info->result.contiguous--;
  }

  // If more pages must be freed, do so now
  if(info->result.contiguous) {
    storfs_page_t free_count = 0;

    err = bitmap_free_contiguous(fs,
                                 info->result.location.pageLoc,
                                 &free_count,
                                 info->result.contiguous);
    if(err != STORFS_OK) {
      return err;
    }
    processed_bytes += fs->pageSize * info->result.contiguous;
  }

  info->processed.bytes += processed_bytes;

  // It is important to erase extents at the end to avoid orphaning data pages
  err = erase_handle_extents(fs, info, node);

  return err;
}

storfs_err_t
snode_erase_data(storfs_t *fs, storfs_page_t page, storfs_byte_t offset) {

  SNodeOpCtx ctx = {
    .page   = page,
    .offset = offset,
    .cb     = snode_erase_op,
    .data   = NULL,
    .size   = 0,
    .op     = ERASE,
  };

  return snode_op(fs, &ctx);
}
