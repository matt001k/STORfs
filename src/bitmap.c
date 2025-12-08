#include "bitmap.h"

#include "core.h"

#include <string.h>

#define BITMAP_PAGE_OFFSET        1
#define STORFS_PROTECTED_PAGES(f) (f->bitmap.page_count + BITMAP_PAGE_OFFSET)
#define PAGE_NO_ALLOC             0

static storfs_err_t write_op(storfs_t *fs, storfs_page_t page) {
  // TODO: replace with atomic operation
  if(fs->erase(fs, page) != STORFS_OK) {
    return STORFS_ERR_ERASE_FAILED;
  }

  if(fs->write(fs, page, 0, fs->buf, fs->pageSize) != STORFS_OK) {
    return STORFS_ERR_WRITE_FAILED;
  }

  return STORFS_OK;
}

static storfs_err_t
read_bitmap_page(storfs_t *fs, storfs_page_t page, uint32_t *byte) {
  uint32_t byte_offset = DIV_BY_8(page);
  uint32_t page_offset = byte_offset / fs->pageSize;

  if(fs->read(fs, page_offset + BITMAP_PAGE_OFFSET, 0, fs->buf, fs->pageSize) !=
     STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }

  if(byte) {
    *byte = byte_offset % fs->pageSize;
  }

  return STORFS_OK;
}

static inline storfs_err_t write_bitmap_page(storfs_t *fs, storfs_page_t page) {
  uint32_t byte_offset = DIV_BY_8(page);
  uint32_t page_offset = byte_offset / fs->pageSize;

  return write_op(fs, page_offset + BITMAP_PAGE_OFFSET);
}

static storfs_err_t get_byte(storfs_t *fs, storfs_page_t page, uint8_t **byte) {
  uint32_t     byte_offset;
  storfs_err_t err = read_bitmap_page(fs, page, &byte_offset);

  if(err != STORFS_OK) {
    return err;
  }

  *byte = &fs->buf[byte_offset];

  return STORFS_OK;
}

static void set_alloc(uint8_t page, uint8_t *byte, uint8_t alloc) {
  uint8_t bit  = page % 8;
  uint8_t mask = 1 << bit;

  if(alloc) {
    *byte |= mask;
  } else {
    *byte &= ~mask;
  }
}

static uint8_t get_alloc(uint8_t page, uint8_t byte) {
  uint8_t bit  = page % 8;
  uint8_t mask = 1 << bit;

  if(byte & mask) {
    return PAGE_ALLOC;
  } else {
    return PAGE_FREE;
  }
}

static storfs_err_t
find_next_available_page(storfs_t *fs, uint8_t alloc, storfs_page_t *page) {
  uint8_t     *buf   = NULL;
  storfs_err_t err   = STORFS_OK;
  uint32_t     start = fs->bitmap.hint;
  uint32_t     candidate;
  uint8_t     *byte;
  uint32_t     i = 0;

  for(; i < fs->pageCount; i++) {
    candidate = (start + i) % fs->pageCount;

    if(!(candidate % fs->pageSize)) {
      buf = NULL;
    }

    if(!buf) {
      err = read_bitmap_page(fs, candidate, NULL);
      buf = fs->buf;
    }

    if(err != STORFS_OK) {
      break;
    }

    uint32_t byte_offset = DIV_BY_8(candidate) % fs->pageSize;
    byte                 = &buf[byte_offset];

    if(!get_alloc(candidate, *byte)) {
      break;
    }
  }

  if(i == fs->pageCount) {
    err = STORFS_ERR_NO_SPACE;
  }

  if(err == STORFS_OK) {
    if(page) {
      *page = candidate;
    }

    if(alloc) {
      set_alloc(candidate, byte, PAGE_ALLOC);
      err             = write_bitmap_page(fs, candidate);
      fs->bitmap.hint = ++candidate % fs->pageCount;
    } else {
      fs->bitmap.hint = candidate;
    }
  }

  return err;
}

static inline void init_struct(storfs_t *fs) {
  fs->bitmap.page_count = CEIL_DIV(DIV_BY_8(fs->pageCount), fs->pageSize);
  fs->bitmap.hint       = STORFS_PROTECTED_PAGES(fs);
}

storfs_err_t bitmap_init(storfs_t *fs) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  init_struct(fs);
  return find_next_available_page(fs, 0, NULL);
}

storfs_err_t bitmap_create(storfs_t *fs) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_err_t   err           = STORFS_OK;
  const uint32_t bits_per_page = MULT_BY_8(fs->pageSize);

  init_struct(fs);

  memset(fs->buf, 0, fs->pageSize);

  for(uint32_t i = 0; i < fs->pageCount;) {
    uint32_t byte_index = DIV_BY_8(i) % fs->pageSize;
    uint32_t bit_index  = i % 8;

    if(i < STORFS_PROTECTED_PAGES(fs)) {
      fs->buf[byte_index] |= (1 << bit_index);
    } else {
      fs->buf[byte_index] &= ~(1 << bit_index);
    }
    i++;

    if(!(i % bits_per_page) || i == fs->pageCount) {
      uint32_t page = CEIL_DIV(i, bits_per_page);
      err           = write_op(fs, page);
      memset(fs->buf, 0, fs->pageSize);
    }

    if(err != STORFS_OK) {
      return err;
    }
  }

  return STORFS_OK;
}

storfs_err_t bitmap_find_next(storfs_t *fs, storfs_page_t *page) {
  if(!fs || !page) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_next_available_page(fs, PAGE_NO_ALLOC, page);
}

storfs_err_t bitmap_alloc(storfs_t *fs, storfs_page_t *page) {
  if(!fs || !page) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_next_available_page(fs, PAGE_ALLOC, page);
}

storfs_err_t bitmap_free(storfs_t *fs, storfs_page_t page) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  if(page < STORFS_PROTECTED_PAGES(fs)) {
    return STORFS_ERR_INVALID_PARAM;
  }

  uint8_t     *byte = NULL;
  storfs_err_t err  = get_byte(fs, page, &byte);

  if(err != STORFS_OK) {
    return err;
  }

  set_alloc(page, byte, PAGE_FREE);

  return write_op(fs, page);
}

storfs_err_t
bitmap_get_alloc(storfs_t *fs, storfs_page_t page, uint8_t *alloc) {
  if(!fs || !alloc) {
    return STORFS_ERR_NULL_POINTER;
  }

  uint8_t     *byte = NULL;
  storfs_err_t err  = get_byte(fs, page, &byte);

  if(err != STORFS_OK) {
    return err;
  }

  *alloc = get_alloc(page, *byte);

  return STORFS_OK;
}
