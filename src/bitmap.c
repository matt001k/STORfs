#include "bitmap.h"

#include "atomic.h"
#include "common.h"

#include <stdbool.h>
#include <string.h>

#define BITMAP_PAGE_OFFSET        1
#define STORFS_PROTECTED_PAGES(f) (f->bitmap.page_count + BITMAP_PAGE_OFFSET)

typedef enum {
  GET_FREE,
  GET_ALLOCATED,
  ALLOC,
  FREE,
} BitmapAllocAction;

typedef enum {
  BITMAP_CONTINUE,
  BITMAP_STOP,
} BitmapContiguousIter;

typedef struct {
  struct {
    storfs_page_t     page;
    storfs_page_t     max;
    BitmapAllocAction action;
  } request;
  struct {
    storfs_page_t count;
    bool          modified;
  } tracking;
} BitmapContiguousInfo;

static storfs_err_t
read_bitmap_page(storfs_t *fs, storfs_page_t page, uint32_t *byte) {
  uint32_t byte_offset = DIV_BY_8(page);
  uint32_t page_offset = byte_offset / fs->pageSize;

  if(fs->read(fs,
              page_offset + BITMAP_PAGE_OFFSET,
              0,
              fs->working_buf,
              fs->pageSize) != STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }

  *byte = byte_offset % fs->pageSize;

  return STORFS_OK;
}

static inline storfs_err_t write_bitmap_page(storfs_t *fs, storfs_page_t page) {
  uint32_t byte_offset = DIV_BY_8(page);
  uint32_t page_offset = byte_offset / fs->pageSize;

  return atomic_write(fs, page_offset + BITMAP_PAGE_OFFSET);
}

static storfs_err_t get_byte(storfs_t *fs, storfs_page_t page, uint8_t **byte) {
  uint32_t     byte_offset;
  storfs_err_t err = read_bitmap_page(fs, page, &byte_offset);

  if(err != STORFS_OK) {
    return err;
  }

  *byte = &fs->working_buf[byte_offset];

  return STORFS_OK;
}

static inline uint8_t get_bit_offset(storfs_page_t page) {
  return page % 8;
}

static inline uint8_t get_alloc(storfs_page_t page, uint8_t byte) {
  return (byte >> (get_bit_offset(page))) & 1;
}

static inline bool can_skip_byte(uint8_t byte, BitmapAllocAction action) {
  switch(action) {
    case GET_ALLOCATED:
    case FREE:
      return byte == 0xFF;
    case GET_FREE:
    case ALLOC:
      return byte == 0x00;
  }

  return false;
}

static inline bool expects_allocated(BitmapAllocAction action) {
  return action == GET_ALLOCATED || action == FREE;
}

static inline void modify_full_byte(uint8_t              *byte,
                                    BitmapAllocAction     action,
                                    BitmapContiguousInfo *info) {
  if(action == ALLOC) {
    *byte                   = 0xFF;
    info->tracking.modified = true;
  } else if(action == FREE) {
    *byte                   = 0x00;
    info->tracking.modified = true;
  }
}

static inline void modify_bit(uint8_t              *byte,
                              uint8_t               mask,
                              BitmapAllocAction     action,
                              BitmapContiguousInfo *info) {

  if(action == ALLOC) {
    *byte |= mask;
    info->tracking.modified = true;
  } else if(action == FREE) {
    *byte &= ~mask;
    info->tracking.modified = true;
  }
}

static BitmapContiguousIter process_bitmap_page(storfs_t             *fs,
                                                BitmapContiguousInfo *info,
                                                storfs_page_t        *page,
                                                storfs_byte_t byte_offset) {
  storfs_page_t     remaining      = info->request.max - info->tracking.count;
  BitmapAllocAction action         = info->request.action;
  bool              want_allocated = expects_allocated(action);

  while(byte_offset < fs->pageSize && remaining > 0) {
    uint8_t *byte = &fs->working_buf[byte_offset];
    uint8_t  bit  = get_bit_offset(*page);

    // Byte-level processing - skip/modify full bytes when aligned
    if(bit == 0 && remaining >= 8 && can_skip_byte(*byte, action)) {
      modify_full_byte(byte, action, info);

      info->tracking.count += 8;
      remaining -= 8;
      *page = (*page + 8) % fs->pageCount;
      byte_offset++;

      if(*page == 0) {
        return BITMAP_CONTINUE;
      }
      continue;
    }

    // Bit-level processing
    uint8_t mask         = 1 << bit;
    bool    is_allocated = *byte & mask;

    if(is_allocated != want_allocated) {
      return BITMAP_STOP;
    }

    modify_bit(byte, mask, action, info);

    info->tracking.count++;
    remaining--;
    *page = (*page + 1) % fs->pageCount;

    if(*page == 0) {
      return BITMAP_CONTINUE;
    }

    if(get_bit_offset(*page) == 0) {
      byte_offset++;
    }
  }

  return BITMAP_CONTINUE;
}

static storfs_err_t get_set_contiguous_pages(storfs_t             *fs,
                                             BitmapContiguousInfo *info) {
  storfs_err_t         err    = STORFS_OK;
  storfs_page_t        page   = info->request.page;
  BitmapContiguousIter result = BITMAP_CONTINUE;

  info->tracking.count = 0;

  while(info->tracking.count < info->request.max && result == BITMAP_CONTINUE) {
    uint32_t      byte_offset;
    storfs_page_t page_at_read = page;

    err = read_bitmap_page(fs, page, &byte_offset);
    if(err != STORFS_OK) {
      return err;
    }

    result = process_bitmap_page(fs, info, &page, byte_offset);

    if(info->tracking.modified) {
      err = write_bitmap_page(fs, page_at_read);
      if(err != STORFS_OK) {
        return err;
      }
      info->tracking.modified = false;
    }
  }

  // Only update hint if pages actually found
  if(info->tracking.count) {
    if(info->request.action == ALLOC) {
      fs->bitmap.hint = page;
    } else if(info->request.action == FREE) {
      fs->bitmap.hint = info->request.page;
    } else if(info->request.action == GET_ALLOCATED) {
      fs->bitmap.hint =
          (info->request.page + info->tracking.count) % fs->pageCount;
    }
  }

  return STORFS_OK;
}

static storfs_err_t find_next_available_page(storfs_t      *fs,
                                             bool           should_allocate,
                                             storfs_page_t *page) {
  uint32_t start = fs->bitmap.hint;
  BitmapContiguousInfo info = {
    .request = {
      .page = start,
      .max = fs->pageCount,
      .action = GET_ALLOCATED,
    },
    .tracking = {
      .count = 0,
      .modified = false,
    },
  };
  storfs_err_t err = get_set_contiguous_pages(fs, &info);
  if(err != STORFS_OK) {
    return err;
  }

  if(info.tracking.count == fs->pageCount) {
    return STORFS_ERR_NO_FREE_BLOCKS;
  }

  if(page) {
    *page = fs->bitmap.hint;
  }

  if(should_allocate) {
    // Allocate via get_set_contiguous_pages with max=1
    BitmapContiguousInfo alloc_info = {                                                                                                                                                                                                                                  
      .request = {                                                                                                                                                                                                                                                       
        .page = fs->bitmap.hint,                                                                                                                                                                                                                                               
        .max = 1,                                                                                                                                                                                                                                                        
        .action = ALLOC,                                                                                                                                                                                                                                                 
      },                                                                                                                                                                                                                                                                 
      .tracking = {                                                                                                                                                                                                                                                      
        .count = 0,                                                                                                                                                                                                                                                      
        .modified = false,                                                                                                                                                                                                                                               
      },                                                                                                                                                                                                                                                                 
    };
    err = get_set_contiguous_pages(fs, &alloc_info);
  }

  return err;
}

static storfs_err_t bitmap_contiguous_op(storfs_t         *fs,
                                         storfs_page_t    *page,
                                         storfs_page_t    *count,
                                         storfs_page_t     max,
                                         BitmapAllocAction action) {

  if(!fs || !count || !page) {
    return STORFS_ERR_NULL_POINTER;
  }

  if(!max || max > fs->pageCount) {
    return STORFS_ERR_INVALID_PARAM;
  }

  storfs_err_t  err         = STORFS_OK;
  storfs_page_t pages_found = 0;

  // First find the next available page if allocing or getting
  if(action != FREE) {
    bool alloc_page = action == ALLOC;
    err             = find_next_available_page(fs, alloc_page, page);
    if(err != STORFS_OK) {
      return err;
    }

    // First page has been found, find the next max contiguous pages
    pages_found = 1;
  }

  BitmapContiguousInfo info = {
    .request = {
      .page = *page + pages_found,
      .max = max - pages_found,
      .action = action,
    },
    .tracking = {
      .count = 0,
      .modified = false,
    }
  };
  err = get_set_contiguous_pages(fs, &info);
  if(err == STORFS_OK) {
    *count = pages_found + info.tracking.count;
  }

  return err;
}

static inline void init_struct(storfs_t *fs) {
  fs->bitmap.page_count = CEIL_DIV(fs->pageCount, MULT_BY_8(fs->pageSize));
  fs->bitmap.hint       = STORFS_PROTECTED_PAGES(fs);
}

/*!
 @brief Initialize the bitmap

 @details Initializes the bitmap and finds the initial hint to
          improve searching when allocating pages.

 @param fs pointer to the filesystem instance

 @return STORFS_ERR_NULL_POINTER if fs is NULL
         STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_init(storfs_t *fs) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  init_struct(fs);
  return find_next_available_page(fs, false, NULL);
}

/*!
 @brief Create the bitmap

 @details This is intended to be called when the filesystem is first created.
          It will initialize all pages as free besides the pages reserved for
          the root page and bitmap.

 @param fs pointer to the filesystem instance

 @return STORFS_ERR_NULL_POINTER if fs is NULL
         STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_create(storfs_t *fs) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_err_t   err           = STORFS_OK;
  const uint32_t bits_per_page = MULT_BY_8(fs->pageSize);

  init_struct(fs);

  memset(fs->working_buf, 0, fs->pageSize);

  for(uint32_t i = 0; i < fs->pageCount; i++) {
    uint32_t byte_index = DIV_BY_8(i) % fs->pageSize;
    uint32_t bit_index  = i % 8;

    // Protected pages are the root page and bitmap pages
    if(i < STORFS_PROTECTED_PAGES(fs)) {
      fs->working_buf[byte_index] |= (1 << bit_index);
    }

    bool is_page_boundary = ((i + 1) % bits_per_page == 0);
    bool is_last_bit      = (i + 1 == fs->pageCount);

    if(is_page_boundary || is_last_bit) {
      uint32_t page = (i / bits_per_page) + 1;
      err           = atomic_write(fs, page);
      if(err != STORFS_OK) {
        return err;
      }
      memset(fs->working_buf, 0, fs->pageSize);
    }
  }

  return STORFS_OK;
}

/*!
 @brief Find the next free page

 @details Will search the bitmap until the next free page (bit set to 0) is
          found. Returns the page through pointer.

 @param fs pointer to the filesystem instance
 @param page pointer assigned to next free page

 @return STORFS_ERR_NULL_POINTER if fs is NULL
         STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_find_next(storfs_t *fs, storfs_page_t *page) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_next_available_page(fs, false, page);
}

/*!
 @brief Allocate the next free page

 @details Will search the bitmap until the next free page (bit set to 0) is
          found. Returns the page through pointer. Sets the page as allocated.

 @param fs pointer to the filesystem instance
 @param page pointer assigned to allocated free page

 @return STORFS_ERR_NULL_POINTER if fs is NULL
         STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_alloc(storfs_t *fs, storfs_page_t *page) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  return find_next_available_page(fs, true, page);
}

/*!
 @brief Allocate or free a specified page

 @details Allocates or frees the specified page by setting/clearing its bit in
 the bitmap. Validates that the page is not protected and is within bounds.

 @param fs pointer to the filesystem instance
 @param page the page number to free

 @return STORFS_ERR_NULL_POINTER if fs is NULL
         STORFS_ERR_INVALID_PARAM if page is protected or exceeds page count
         STORFS_OK on success
         Other error upon failure
 */
storfs_err_t
bitmap_alloc_page(storfs_t *fs, storfs_page_t page, uint8_t alloc) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  if(page < STORFS_PROTECTED_PAGES(fs) || page >= fs->pageCount) {
    return STORFS_ERR_INVALID_PARAM;
  }

  uint8_t     *byte = NULL;
  storfs_err_t err  = get_byte(fs, page, &byte);

  if(err != STORFS_OK) {
    return err;
  }

  uint8_t bit  = get_bit_offset(page);
  uint8_t mask = 1 << bit;

  if(alloc == PAGE_FREE) {
    *byte &= ~mask;
  } else {
    *byte |= mask;
  }

  err = write_bitmap_page(fs, page);
  if(err != STORFS_OK) {
    return err;
  }

  return bitmap_find_next(fs, NULL);
}

/*!
 @brief Get the allocation status of a page

 @details Determines if a specific page has been allocated.

 @param fs pointer to the filesystem instance
 @param page page to check if it has been allocated
 @param alloc pointer set to PAGE_ALLOC if allocated, PAGE_FREE if free

 @return STORFS_ERR_NULL_POINTER if fs or alloc is NULL
         STORFS_ERR_INVALID_PARAM if page is larger than the number of pages
         STORFS_OK on success
         Other error upon failure
 */
storfs_err_t
bitmap_get_alloc(storfs_t *fs, storfs_page_t page, uint8_t *alloc) {
  if(!fs || !alloc) {
    return STORFS_ERR_NULL_POINTER;
  }

  if(page >= fs->pageCount) {
    return STORFS_ERR_INVALID_PARAM;
  }

  uint8_t     *byte = NULL;
  storfs_err_t err  = get_byte(fs, page, &byte);
  if(err != STORFS_OK) {
    return err;
  }

  *alloc = get_alloc(page, *byte);

  return STORFS_OK;
}

/*!
 @brief Find the number of contiguous free pages

 @details Will find the next available page and determine how many pages are
          free following that page up to a maximum amount.

 @param fs pointer to the filesystem instance
 @param page pointer assigned to next free page
 @param count pointer to number of contiguous pages
 @param max max number of contiguous free pages to find

 @return STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_get_contiguous(storfs_t      *fs,
                                   storfs_page_t *page,
                                   storfs_page_t *count,
                                   storfs_page_t  max) {
  return bitmap_contiguous_op(fs, page, count, max, GET_FREE);
}

/*!
 @brief Allocate a number of contiguous pages

 @details Finds the next available page and attempts to allocate up to max
          contiguous pages starting from that page.

 @param fs pointer to the filesystem instance
 @param page pointer assigned to first allocated page
 @param count pointer to number of allocated pages
 @param max max number of pages that can be allocated in a row

 @return STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_alloc_contiguous(storfs_t      *fs,
                                     storfs_page_t *page,
                                     storfs_page_t *count,
                                     storfs_page_t  max) {

  return bitmap_contiguous_op(fs, page, count, max, ALLOC);
}

/*!
 @brief Free a number of contiguous allocated pages

 @details Frees contiguous allocated pages starting from the indicated page,
          up to a maximum of max pages. Stops early if a free page is
          encountered.

 @param fs pointer to the filesystem instance
 @param page page number to begin freeing
 @param count pointer to number of pages freed
 @param max max number of pages that can be freed in a row

 @return STORFS_OK on success
         Other error upon failure
 */
storfs_err_t bitmap_free_contiguous(storfs_t      *fs,
                                    storfs_page_t  page,
                                    storfs_page_t *count,
                                    storfs_page_t  max) {

  if(fs && page < STORFS_PROTECTED_PAGES(fs)) {
    return STORFS_ERR_INVALID_PARAM;
  }

  return bitmap_contiguous_op(fs, &page, count, max, FREE);
}
