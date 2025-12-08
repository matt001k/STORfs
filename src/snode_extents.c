#include "snode_extents.h"

#include "atomic.h"
#include "bitmap.h"
#include "core.h"
#include "crc.h"

#include <stdbool.h>
#include <string.h>

#define EXTENTS_PER_PAGE(f) ((f->pageSize / sizeof(SNodeExtent)))
#define SINGLE_INDIRECT_DATA_CALC(f)                                           \
  (f->pageSize * DATA_PAGES_PER_INDIRECT_PAGE(f))

#define INLINE_DATA_SIZE(f) (f->pageSize - sizeof(SNode))

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
    storfs_byte_t bytes;  // bytes processed in the operation (SNodeOpCb)
    storfs_page_t pages;  // bytes processed in the operation (SNodeOpCb)
  } processed;
  struct {
    storfs_page_t extent_pages;
  } tracking;
} SNodeLocationInfo;

struct SNodeOpCtx;

typedef storfs_err_t (*SNodeOpCb)(storfs_t                *fs,
                                  SNodeLocationInfo       *info,
                                  SNode                   *node,
                                  const struct SNodeOpCtx *ctx);

typedef struct SNodeOpCtx {
  storfs_page_t page;         // SNode page location
  storfs_byte_t offset;       // Offset of SNode data
  SNodeOpCb     cb;           // Operation callback
  uint8_t      *data;         // Data point for SNode operation
  uint32_t      size;         // Size of data
  SNodeOp       op;           // Operation to perform on SNode
  uint8_t       appends : 1;  // Whether operation appends to SNode
  uint8_t updates_snode : 1;  // Whether the SNode is update from the operation
  uint8_t : 6;
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

  memset(fs->buf, 0, fs->pageSize);

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

static inline storfs_page_t calculate_contiguous(SNodeLocationInfo info,
                                                 storfs_loc_t      logical,
                                                 SNodeExtent       extent) {

  storfs_page_t offset_in_extent = logical.pageLoc - info.tracking.extent_pages;
  storfs_page_t remaining_in_extent = extent.count - offset_in_extent;
  return MIN(info.request.max, remaining_in_extent);
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

  err = snode_check_read(fs, indirect_page, 0, fs->buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  SNodeExtent *extents      = (SNodeExtent *)fs->buf;
  uint32_t     extent_count = EXTENTS_PER_PAGE(fs);

  err = find_page_in_extents(extents,
                             &extent_count,
                             logical.pageLoc,
                             info,
                             &physical_page);

  if(err != STORFS_OK && info->request.op == WRITE &&
     extent_count < EXTENTS_PER_PAGE(fs)) {
    err = bitmap_alloc_contiguous(fs,
                                  &physical_page,
                                  &info->result.contiguous,
                                  info->request.max);
    if(err != STORFS_OK) {
      return err;
    }

    // Must read back in the original page
    err = snode_check_read(fs, indirect_page, 0, fs->buf, fs->pageSize);
    if(err != STORFS_OK) {
      return err;
    }

    // Pointer extents still points to fs->buf
    extents[extent_count].start = physical_page;
    extents[extent_count].count = info->result.contiguous;

    err = atomic_write(fs, indirect_page);

    if(alloc) {
      *alloc = true;
    }
  }

  if(err != STORFS_OK) {
    return err;
  }

  info->result.contiguous =
      calculate_contiguous(*info, logical, extents[extent_count]);
  info->result.location.pageLoc = physical_page;
  info->result.location.byteLoc = logical.byteLoc;
  return STORFS_OK;
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

  // Try direct extents first
  uint32_t extent_count = ARRAY_SIZE(node->direct);
  storfs_err_t err = find_page_in_extents(node->direct,
                                          &extent_count,
                                          logical.pageLoc,
                                          info,
                                          &physical_page);

  if(err != STORFS_OK && info->request.op == WRITE &&
     extent_count < ARRAY_SIZE(node->direct)) {
    err = bitmap_alloc_contiguous(fs,
                                  &physical_page,
                                  &info->result.contiguous,
                                  info->request.max);
    if(err != STORFS_OK) {
      return err;
    }
    // Page will get saved at the end of operation
    node->direct[extent_count].start = physical_page;
    node->direct[extent_count].count = info->result.contiguous;
  }

  if(err == STORFS_OK) {
    info->result.location.pageLoc = physical_page;
    info->result.location.byteLoc = logical.byteLoc;
    info->result.contiguous =
        calculate_contiguous(*info, logical, node->direct[extent_count]);
    return STORFS_OK;
  }

  return STORFS_ERR_NOT_FOUND;
}

static inline storfs_err_t
find_single_indirect_location(storfs_t          *fs,
                              SNode             *node,
                              SNodeLocationInfo *info,
                              storfs_loc_t      *logical) {

  storfs_err_t err =
      ensure_node_page_allocated(fs, &node->indirect.single, info);
  if(err != STORFS_OK) {
    return err;
  }

  err = STORFS_ERR_BAD_BLOCK;
  if(node->indirect.single) {
    err = find_extent_in_indirect_page(fs,
                                       node->indirect.single,
                                       info,
                                       *logical,
                                       NULL);
  }

  return err;
}

static inline storfs_err_t
find_double_indirect_location(storfs_t          *fs,
                              SNode             *node,
                              SNodeLocationInfo *info,
                              storfs_loc_t      *logical) {
  storfs_page_t multiple_pre_alloc = node->indirect.multiple;
  storfs_err_t  err =
      ensure_node_page_allocated(fs, &node->indirect.multiple, info);
  if(err != STORFS_OK) {
    return err;
  }

  if(node->indirect.multiple) {
    err =
        snode_check_read(fs, node->indirect.multiple, 0, fs->buf, fs->pageSize);
    if(err != STORFS_OK) {
      return err;
    }

    SNodeExtent   *extents                  = (SNodeExtent *)fs->buf;
    storfs_page_t *pages_seen               = &info->tracking.extent_pages;
    storfs_page_t  single_indirect_location = 0;

    uint32_t i = 0;
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

    // Try extending last entry before allocating new single indirect page
    if(!single_indirect_location && i > 0 && info->request.op == WRITE) {
      i--;
      single_indirect_location    = extents[i].start;
      info->tracking.extent_pages = *pages_seen - extents[i].count;
    }

    if(!single_indirect_location && info->request.op == WRITE) {
      err = alloc_page(fs, &single_indirect_location);
      if(err != STORFS_OK) {
        return err;
      }
    }

    bool alloc = false;
    err        = find_extent_in_indirect_page(fs,
                                       single_indirect_location,
                                       info,
                                       *logical,
                                       &alloc);

    // Last entry was full, allocate new single indirect page
    if(err == STORFS_ERR_NOT_FOUND && info->request.op == WRITE) {
      i++;
      err = alloc_page(fs, &single_indirect_location);
      if(err == STORFS_OK) {
        err = find_extent_in_indirect_page(fs,
                                           single_indirect_location,
                                           info,
                                           *logical,
                                           &alloc);
      }
    }

    if(err != STORFS_OK) {
      return err;
    }

    if(alloc) {
      err = snode_check_read(fs,
                             node->indirect.multiple,
                             0,
                             fs->buf,
                             fs->pageSize);
      if(err != STORFS_OK) {
        return err;
      }

      extents          = (SNodeExtent *)fs->buf;
      extents[i].start = single_indirect_location;
      extents[i].count += info->result.contiguous;
      err = atomic_write(fs, node->indirect.multiple);
    }
  }

  return err;
}

static storfs_err_t
find_data_location(storfs_t *fs, SNode *node, SNodeLocationInfo *info) {

  if(info->request.op != WRITE && info->request.offset >= node->size) {
    return STORFS_ERR_INVALID_PARAM;
  }

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

  if(info->request.op == WRITE || info->request.op == READ) {
    err = snode_check_read(fs,
                           info->result.location.pageLoc,
                           0,
                           fs->buf,
                           fs->pageSize);
  }

  return err;
}

static storfs_err_t
snode_update(storfs_t *fs, SNode *node, storfs_page_t page) {
  storfs_err_t err = snode_check_read(fs, page, 0, fs->buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  node->crc = 0;
  node->crc = storfs_crc16((const uint8_t *)node, sizeof(SNode));

  SNode *write_node = (SNode *)fs->buf;
  *write_node       = *node;

  return atomic_write(fs, page);
}

static storfs_err_t snode_op(storfs_t *fs, const SNodeOpCtx *ctx) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node;
  storfs_err_t err = snode_lookup(fs, ctx->page, &node);

  SNodeLocationInfo info = { 0 };
  while(err == STORFS_OK && info.processed.bytes < ctx->size) {
    if(ctx->appends) {
      info.request.offset = node.size;
    } else {
      info.request.offset = info.processed.bytes + ctx->offset;
    }
    info.request.page = ctx->page;
    info.request.op   = ctx->op;
    info.request.max = CEIL_DIV(ctx->size - info.processed.bytes, fs->pageSize);
    info.tracking.extent_pages = 0;

    err = get_location_info(fs, &node, &info);
    if(err != STORFS_OK) {
      break;
    }

    info.processed.pages = 0;
    while(info.processed.pages < info.result.contiguous) {
      err = ctx->cb(fs, &info, &node, ctx);
      if(err != STORFS_OK) {
        break;
      }
      info.processed.pages++;
    }
  }

  if(ctx->updates_snode && info.processed.bytes) {
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

  return bitmap_alloc(fs, NULL);
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

  uint32_t page_size_left  = fs->pageSize - info->result.location.byteLoc;
  uint32_t bytes_remaining = ctx->size - info->processed.bytes;
  uint32_t write_size      = MIN(bytes_remaining, page_size_left);

  // Zero buffer when starting at page boundary
  if(!info->result.location.byteLoc) {
    memset(fs->buf, 0, fs->pageSize);
  }
  memcpy(&fs->buf[info->result.location.byteLoc],
         &ctx->data[info->processed.bytes],
         write_size);

  storfs_err_t err = atomic_write(fs, info->result.location.pageLoc);
  if(err == STORFS_OK) {
    info->result.location.byteLoc = 0;
    info->result.location.pageLoc++;
    node->size += write_size;
    info->processed.bytes += write_size;
  }

  return err;
}

storfs_err_t snode_write_data(storfs_t      *fs,
                              storfs_page_t  page,
                              const uint8_t *data,
                              uint32_t       size) {
  SNodeOpCtx ctx = {
    .page          = page,
    .offset        = 0,
    .cb            = snode_write_op,
    .data          = (uint8_t *)data,
    .size          = size,
    .op            = WRITE,
    .appends       = true,
    .updates_snode = true,
  };


  return snode_op(fs, &ctx);
}

static storfs_err_t snode_read_op(storfs_t          *fs,
                                  SNodeLocationInfo *info,
                                  SNode             *node,
                                  const SNodeOpCtx  *ctx) {
  (void)node;

  storfs_err_t err             = STORFS_OK;
  uint32_t     page_size_left  = fs->pageSize - info->result.location.byteLoc;
  uint32_t     bytes_remaining = ctx->size - info->processed.bytes;
  uint32_t     read_size       = MIN(bytes_remaining, page_size_left);
  memcpy(&ctx->data[info->processed.bytes],
         &fs->buf[info->result.location.byteLoc],
         read_size);

  info->result.location.byteLoc = 0;
  info->result.location.pageLoc++;
  info->processed.bytes += read_size;

  if(info->result.contiguous - info->processed.pages) {
    err = snode_check_read(fs,
                           info->result.location.pageLoc,
                           0,
                           fs->buf,
                           fs->pageSize);
  }

  return err;
}

storfs_err_t snode_read_data(storfs_t     *fs,
                             storfs_page_t page,
                             storfs_byte_t offset,
                             uint8_t      *data,
                             uint32_t      size) {
  SNodeOpCtx ctx = {
    .page          = page,
    .offset        = offset,
    .cb            = snode_read_op,
    .data          = (uint8_t *)data,
    .size          = size,
    .op            = READ,
    .appends       = false,
    .updates_snode = false,
  };

  return snode_op(fs, &ctx);
}

storfs_err_t snode_erase_data(storfs_t     *fs,
                              storfs_page_t page,
                              storfs_byte_t offset,
                              uint32_t      size) {
  // TODO: erase pages starting here

  return STORFS_OK;
}
