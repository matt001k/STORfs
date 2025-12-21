#include "snode.h"

#include "atomic.h"
#include "bitmap.h"
#include "core.h"
#include "crc.h"

#include <string.h>

#define DATA_PAGES_PER_INDIRECT_PAGE(f) ((f->pageSize / sizeof(uint32_t)))
#define SINGLE_INDIRECT_DATA_CALC(f)                                           \
  (f->pageSize * DATA_PAGES_PER_INDIRECT_PAGE(f))

#define PAGE_DATA_SIZE(f) (f->pageSize - sizeof(SNode))
#define DIRECT_DATA_SIZE(f, s)                                                 \
  (PAGE_DATA_SIZE(f) + (f->pageSize * ARRAY_SIZE(s->direct)))
#define SINGLE_INDIRECT_DATA_SIZE(f, s)                                        \
  (DIRECT_DATA_SIZE(f, s) + SINGLE_INDIRECT_DATA_CALC(f))
#define MULTIPLE_INDIRECT_DATA_SIZE(f, s)                                      \
  (SINGLE_INDIRECT_DATA_SIZE(f, s) +                                           \
   (SINGLE_INDIRECT_DATA_CALC(f) * DATA_PAGES_PER_INDIRECT_PAGE(f)))

typedef enum {
  WRITE,
  READ,
  ERASE,
} SNodeOp;

typedef struct {
  storfs_loc_t  location;
  storfs_page_t page;
  storfs_byte_t offset;
  SNodeOp       op;
} SNodeLocationInfo;

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

static storfs_err_t alloc_page(storfs_t *fs, storfs_page_t *page) {
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
  if(!*page && info->op == WRITE) {
    // Node structure will get saved at the end of write operation
    err = alloc_page(fs, page);
    if(err != STORFS_OK) {
      return err;
    }
  }

  return STORFS_OK;
}

static storfs_err_t find_indirect_page(storfs_t          *fs,
                                       uint32_t           idx,
                                       storfs_page_t      indirect_location,
                                       SNode             *node,
                                       SNodeLocationInfo *info) {
  storfs_err_t  err;
  storfs_page_t page;

  err = snode_check_read(fs, indirect_location, 0, fs->buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  uint32_t *p_page = (uint32_t *)fs->buf;
  if(!p_page[idx] && info->op == WRITE) {
    err = alloc_page(fs, &page);
    if(err != STORFS_OK) {
      return err;
    }

    err = snode_check_read(fs, indirect_location, 0, fs->buf, fs->pageSize);
    if(err != STORFS_OK) {
      return err;
    }

    p_page[idx] = page;
    atomic_write(fs, indirect_location);
  }

  info->location.pageLoc = p_page[idx];

  return STORFS_OK;
}

static storfs_err_t resolve_single_indirect_location(storfs_t          *fs,
                                                     uint32_t           idx,
                                                     SNode             *node,
                                                     SNodeLocationInfo *info) {
  storfs_err_t err;
  uint32_t     indirect_idx = idx - ARRAY_SIZE(node->direct);

  err = ensure_node_page_allocated(fs, &node->indirect.single, info);
  if(err != STORFS_OK) {
    return err;
  }

  return find_indirect_page(fs,
                            indirect_idx,
                            node->indirect.single,
                            node,
                            info);
}

static storfs_err_t
resolve_multiple_indirect_location(storfs_t          *fs,
                                   uint32_t           idx,
                                   SNode             *node,
                                   SNodeLocationInfo *info) {
  storfs_err_t  err;
  storfs_page_t page;

  err = ensure_node_page_allocated(fs, &node->indirect.multiple, info);
  if(err != STORFS_OK) {
    return err;
  }

  uint32_t pages_before_multiple =
      ARRAY_SIZE(node->direct) + DATA_PAGES_PER_INDIRECT_PAGE(fs);
  uint32_t page_in_multiple = idx - pages_before_multiple;

  uint32_t indirect_page_idx =
      page_in_multiple / DATA_PAGES_PER_INDIRECT_PAGE(fs);
  uint32_t page_in_indirect =
      page_in_multiple % DATA_PAGES_PER_INDIRECT_PAGE(fs);

  err = snode_check_read(fs, node->indirect.multiple, 0, fs->buf, fs->pageSize);
  if(err != STORFS_OK) {
    return err;
  }

  uint32_t *p_indirect_pages = (uint32_t *)fs->buf;

  if(!p_indirect_pages[indirect_page_idx] && info->op == WRITE) {
    err = alloc_page(fs, &page);
    if(err != STORFS_OK) {
      return err;
    }
    err =
        snode_check_read(fs, node->indirect.multiple, 0, fs->buf, fs->pageSize);
    if(err != STORFS_OK) {
      return err;
    }
    p_indirect_pages[indirect_page_idx] = page;
    atomic_write(fs, node->indirect.multiple);
  }
  storfs_page_t single_indirect_page = p_indirect_pages[indirect_page_idx];

  return find_indirect_page(fs,
                            page_in_indirect,
                            single_indirect_page,
                            node,
                            info);
}

static storfs_err_t
find_data_location(storfs_t *fs, SNode *node, SNodeLocationInfo *info) {
  // Location to access data
  uint32_t snode_data_page_size = PAGE_DATA_SIZE(fs);

  if(info->op != WRITE && info->offset >= node->size) {
    return STORFS_ERR_INVALID_PARAM;
  }

  if(info->offset < snode_data_page_size) {
    info->location.pageLoc = info->page;
    info->location.byteLoc = sizeof(SNode) + info->offset;
    return STORFS_OK;
  }

  storfs_err_t err;
  uint32_t     data_beyond_snode = info->offset - snode_data_page_size;
  struct {
    uint32_t idx;
    uint32_t offset;
  } page_position;
  page_position.idx    = data_beyond_snode / fs->pageSize;
  page_position.offset = data_beyond_snode % fs->pageSize;

  if(page_position.idx < ARRAY_SIZE(node->direct)) {
    err =
        ensure_node_page_allocated(fs, &node->direct[page_position.idx], info);
    if(err != STORFS_OK) {
      return err;
    }

    info->location.pageLoc = node->direct[page_position.idx];
  } else if(info->offset < SINGLE_INDIRECT_DATA_SIZE(fs, node)) {
    err = resolve_single_indirect_location(fs, page_position.idx, node, info);
  } else if(info->offset < MULTIPLE_INDIRECT_DATA_SIZE(fs, node)) {
    err = resolve_multiple_indirect_location(fs, page_position.idx, node, info);
  } else {
    return STORFS_ERR_NO_SPACE;
  }

  if(err == STORFS_OK) {
    info->location.byteLoc = page_position.offset;
  }

  return err;
}

static storfs_err_t
get_location_info(storfs_t *fs, SNode *node, SNodeLocationInfo *info) {
  storfs_err_t err = find_data_location(fs, node, info);
  if(err != STORFS_OK) {
    return err;
  }

  return snode_check_read(fs, info->location.pageLoc, 0, fs->buf, fs->pageSize);
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

storfs_err_t snode_write_data(storfs_t      *fs,
                              storfs_page_t  page,
                              const uint8_t *data,
                              uint32_t       size) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node;
  storfs_err_t err = snode_lookup(fs, page, &node);
  if(err != STORFS_OK) {
    return err;
  }

  uint32_t bytes_written = 0;
  while(bytes_written < size) {
    SNodeLocationInfo info = { 0 };
    info.offset            = node.size;
    info.page              = page;
    info.op                = WRITE;

    err = get_location_info(fs, &node, &info);
    if(err != STORFS_OK) {
      break;
    }

    uint32_t page_size_left  = fs->pageSize - info.location.byteLoc;
    uint32_t bytes_remaining = size - bytes_written;
    uint32_t write_size      = MIN(bytes_remaining, page_size_left);
    memcpy(&fs->buf[info.location.byteLoc], &data[bytes_written], write_size);

    err = atomic_write(fs, info.location.pageLoc);
    if(err != STORFS_OK) {
      break;
    }

    node.size += write_size;
    bytes_written += write_size;
  }

  if(bytes_written) {
    // Update snode even on error
    storfs_err_t snode_update_err = snode_update(fs, &node, page);

    if(err == STORFS_OK) {
      err = snode_update_err;
    }
  }

  return err;
}

storfs_err_t snode_read_data(storfs_t     *fs,
                             storfs_page_t page,
                             storfs_byte_t offset,
                             uint8_t      *data,
                             uint32_t      size) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  SNode        node;
  storfs_err_t err = snode_lookup(fs, page, &node);
  if(err != STORFS_OK) {
    return err;
  }

  uint32_t bytes_read = 0;
  while(bytes_read < size) {
    SNodeLocationInfo info = { 0 };
    info.offset            = bytes_read + offset;
    info.page              = page;
    info.op                = READ;

    err = get_location_info(fs, &node, &info);
    if(err != STORFS_OK) {
      break;
    }

    uint32_t page_size_left  = fs->pageSize - info.location.byteLoc;
    uint32_t bytes_remaining = size - bytes_read;
    uint32_t read_size       = MIN(bytes_remaining, page_size_left);
    memcpy(&data[bytes_read], &fs->buf[info.location.byteLoc], read_size);

    bytes_read += read_size;
  }

  return err;
}
