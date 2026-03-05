#include "atomic.h"
#include "bitmap.h"
#include "common.h"
#include "fake_flash.h"
#include "unity.h"

#include <math.h>

static storfs_t *fs;

void setUp(void) {
  fs = fake_storfs_init();
  TEST_ASSERT_EQUAL(bitmap_create(fs), STORFS_OK);
}

void tearDown(void) {}

void test_bitmap_init(void) {
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);
  float page_count = ceil(fs->pageCount / 8.0 / fs->pageSize);
  TEST_ASSERT_EQUAL(fs->bitmap.page_count, (uint32_t)page_count);
  TEST_ASSERT_EQUAL(fs->bitmap.hint, fs->bitmap.page_count + 1);

  // Test bad args
  TEST_ASSERT_EQUAL(bitmap_init(NULL), STORFS_ERR_NULL_POINTER);
}

void test_bitmap_find_and_alloc(void) {
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);

  // Test filling the file system
  storfs_page_t page;
  storfs_page_t hint_original = fs->bitmap.hint;
  for(uint32_t i = hint_original; i < fs->pageCount; i++) {
    TEST_ASSERT_EQUAL(bitmap_find_next(fs, &page), STORFS_OK);
    TEST_ASSERT_EQUAL(page, i);
    TEST_ASSERT_EQUAL(fs->bitmap.hint, i % fs->pageCount);
    TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_OK);
    TEST_ASSERT_EQUAL(page, i);
    TEST_ASSERT_EQUAL(fs->bitmap.hint, (i + 1) % fs->pageCount);
  }

  // Test that the file system is full
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_ERR_NO_FREE_BLOCKS);

  // Test finding page after wrapping around
  storfs_page_t page_offset = fs->pageCount / 2;
  fs->bitmap.hint           = page_offset + 1;
  TEST_ASSERT_EQUAL(bitmap_free(fs, page_offset), STORFS_OK);
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_OK);
  TEST_ASSERT_EQUAL(page, page_offset);

  // Test that the file system is full
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_ERR_NO_FREE_BLOCKS);

  // Test invalid args
  TEST_ASSERT_EQUAL(bitmap_alloc(NULL, &page), STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_find_next(NULL, &page), STORFS_ERR_NULL_POINTER);

  // Test failure to read
  fake_storfs_fail_op(READ, true, 1);
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);
}

void test_bitmap_free(void) {
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);

  // Test filling some items in the file system
  storfs_page_t page;
  storfs_page_t hint_original = fs->bitmap.hint;
  uint32_t      pages_to_fill = fs->pageCount / 2;
  for(uint32_t i = hint_original; i < pages_to_fill; i++) {
    storfs_err_t err = bitmap_alloc(fs, &page);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(page, i);
    TEST_ASSERT_EQUAL(fs->bitmap.hint, (i + 1) % fs->pageCount);
  }

  // Test invalid parameters
  TEST_ASSERT_EQUAL(bitmap_free(fs, hint_original - 1),
                    STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(bitmap_free(fs, 0), STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(bitmap_free(fs, fs->pageCount), STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(bitmap_free(NULL, hint_original), STORFS_ERR_NULL_POINTER);

  // Test freeing all allocated pages
  uint8_t alloc;
  for(uint32_t i = pages_to_fill - 1; i >= hint_original; i--) {
    storfs_err_t err = bitmap_get_alloc(fs, i, &alloc);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_ALLOC);
    err = bitmap_free(fs, i);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    err = bitmap_get_alloc(fs, i, &alloc);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_FREE);
  }

  // Test failure to read
  fake_storfs_fail_op(READ, true, 1);
  TEST_ASSERT_EQUAL(bitmap_free(fs, hint_original), STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);
}

void test_bitmap_get_alloc(void) {
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);

  // Test that all non-protected pages are free
  uint8_t       alloc;
  storfs_page_t hint_original = fs->bitmap.hint;
  for(uint32_t i = hint_original; i < fs->pageCount; i++) {
    storfs_err_t err = bitmap_get_alloc(fs, i, &alloc);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_FREE);
  }

  // Test invalid parameters
  TEST_ASSERT_EQUAL(bitmap_get_alloc(NULL, hint_original, &alloc),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, hint_original, NULL),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, fs->pageCount, &alloc),
                    STORFS_ERR_INVALID_PARAM);

  // Test failure to read
  fake_storfs_fail_op(READ, true, 1);
  TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, hint_original, &alloc),
                    STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);
}

void test_bitmap_create(void) {
  // Test invalid parameters
  TEST_ASSERT_EQUAL(bitmap_create(NULL), STORFS_ERR_NULL_POINTER);

  // Test invalid write operation
  fake_storfs_fail_op(WRITE, true, 1);
  TEST_ASSERT_EQUAL(bitmap_create(fs), STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false, 0);

  // Test invalid erase operation
  fake_storfs_fail_op(ERASE, true, 1);
  TEST_ASSERT_EQUAL(bitmap_create(fs), STORFS_ERR_ERASE_FAILED);
  fake_storfs_fail_op(ERASE, false, 0);
}

void test_bitmap_get_contiguous(void) {
  storfs_err_t err;

  storfs_page_t max = 10;
  storfs_page_t count;
  storfs_page_t page;
  err = bitmap_get_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, max);

  max = fs->pageSize;
  err = bitmap_get_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, max);

  max = fs->pageSize * 5;
  err = bitmap_get_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, max);

  // Test if block is not fully contiguous in accordance to max
  storfs_page_t free_pages = 5;
  storfs_page_t offset     = free_pages + 1;
  for(storfs_page_t i = 0; i < offset; i++) {
    TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_OK);

    // Free blocks will be up to offset - 1
    if(i < offset - 1) {
      TEST_ASSERT_EQUAL(bitmap_free(fs, page), STORFS_OK);
    }
  }
  fs->bitmap.hint = 0;

  err = bitmap_get_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, free_pages);

  // Test failure use cases
  err = bitmap_get_contiguous(NULL, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_NULL_POINTER);
  err = bitmap_get_contiguous(fs, NULL, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_NULL_POINTER);
  err = bitmap_get_contiguous(fs, &page, NULL, max);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_NULL_POINTER);
  err = bitmap_get_contiguous(fs, &page, &count, 0);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_INVALID_PARAM);
  err = bitmap_get_contiguous(fs, &page, &count, fs->pageCount + 1);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_INVALID_PARAM);

  // Test read failure
  fake_storfs_fail_op(READ, true, 1);
  err = bitmap_get_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);
  fake_storfs_fail_op(READ, true, 2);
  err = bitmap_get_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);
}

void test_bitmap_alloc_and_free_contiguous(void) {
  storfs_err_t err;

  storfs_page_t max           = 100;
  storfs_page_t hint_original = fs->bitmap.hint;
  storfs_page_t count;
  storfs_page_t page;
  err = bitmap_alloc_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, max);
  storfs_page_t new_hint = (max + hint_original) % fs->pageCount;
  TEST_ASSERT_EQUAL(fs->bitmap.hint, new_hint);

  storfs_page_t max_page = hint_original + max;
  for(storfs_page_t i = hint_original; i < max_page; i++) {
    uint8_t alloced = 0;
    err             = bitmap_get_alloc(fs, i, &alloced);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(alloced, PAGE_ALLOC);
  }

  err = bitmap_free_contiguous(fs, hint_original, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, max);
  TEST_ASSERT_EQUAL(fs->bitmap.hint, hint_original);

  max_page = hint_original + max;
  for(storfs_page_t i = hint_original; i < max_page; i++) {
    uint8_t alloced = 0;
    err             = bitmap_get_alloc(fs, i, &alloced);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(alloced, PAGE_FREE);
  }

  // Alloc pages and then ensure count is less than max
  fs->bitmap.hint                = hint_original;
  storfs_page_t alloc_page       = 0;
  storfs_page_t contiguous_pages = max - 1;

  for(uint8_t i = 0; i < contiguous_pages; i++) {
    err = bitmap_alloc(fs, &alloc_page);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    err = bitmap_free(fs, alloc_page);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
  }
  err = bitmap_alloc(fs, &alloc_page);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  fs->bitmap.hint = hint_original;

  err = bitmap_alloc_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, contiguous_pages);
  new_hint = (contiguous_pages + hint_original) % fs->pageCount;
  TEST_ASSERT_EQUAL(fs->bitmap.hint, new_hint);

  // Free the pages allocated and check that it is less than max
  err = bitmap_free(fs, alloc_page);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  err = bitmap_free_contiguous(fs, hint_original, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_OK);
  TEST_ASSERT_EQUAL(count, contiguous_pages);
  TEST_ASSERT_EQUAL(fs->bitmap.hint, hint_original);

  // Test write failure
  fake_storfs_fail_op(WRITE, true, 2);
  err = bitmap_alloc_contiguous(fs, &page, &count, max);
  TEST_ASSERT_EQUAL(err, STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false, 0);

  // Test invalid args
  TEST_ASSERT_EQUAL(bitmap_alloc_contiguous(NULL, &page, &count, 10),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_alloc_contiguous(fs, NULL, &count, 10),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_alloc_contiguous(fs, NULL, NULL, 10),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_alloc_contiguous(fs, &page, &count, 0),
                    STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(
      bitmap_alloc_contiguous(fs, &page, &count, fs->pageCount + 1),
      STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(bitmap_free_contiguous(NULL, hint_original, &count, 10),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_free_contiguous(fs, hint_original, NULL, 10),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_free_contiguous(fs, 0, &count, 10),
                    STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(bitmap_free_contiguous(fs, fs->bitmap.hint, &count, 0),
                    STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT_EQUAL(
      bitmap_free_contiguous(fs, fs->bitmap.hint, &count, fs->pageCount + 1),
      STORFS_ERR_INVALID_PARAM);
}

void test_bitmap_create_validates_state(void) {
  // bitmap_create is called in setUp, verify full state
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);

  uint8_t       alloc;
  storfs_page_t protected_pages = fs->bitmap.page_count + 1;

  // Verify all protected pages are allocated
  for(storfs_page_t i = 0; i < protected_pages; i++) {
    TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, i, &alloc), STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_ALLOC);
  }

  // Verify all remaining pages are free
  for(storfs_page_t i = protected_pages; i < fs->pageCount; i++) {
    TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, i, &alloc), STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_FREE);
  }
}

void test_bitmap_double_free(void) {
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);

  storfs_page_t page;
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_OK);

  // First free should succeed
  TEST_ASSERT_EQUAL(bitmap_free(fs, page), STORFS_OK);

  uint8_t alloc;
  TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, page, &alloc), STORFS_OK);
  TEST_ASSERT_EQUAL(alloc, PAGE_FREE);

  // Second free on same page - verify it doesn't corrupt state
  TEST_ASSERT_EQUAL(bitmap_free(fs, page), STORFS_OK);
  TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, page, &alloc), STORFS_OK);
  TEST_ASSERT_EQUAL(alloc, PAGE_FREE);
}

void test_bitmap_alloc_byte_boundary(void) {
  TEST_ASSERT_EQUAL(bitmap_init(fs), STORFS_OK);

  storfs_page_t page;
  storfs_page_t count;
  storfs_page_t hint_original = fs->bitmap.hint;

  // Allocate pages that span a byte boundary (e.g., pages across bits 5-10)
  // First allocate up to 5 bits before a byte boundary
  storfs_page_t pre_boundary = 8 - (hint_original % 8);
  if(pre_boundary == 8) {
    pre_boundary = 5;
  } else if(pre_boundary > 5) {
    pre_boundary -= 3;
  }

  for(storfs_page_t i = 0; i < pre_boundary; i++) {
    TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_OK);
  }

  // Now allocate contiguous pages that cross the byte boundary
  storfs_page_t cross_count = 10;
  TEST_ASSERT_EQUAL(bitmap_alloc_contiguous(fs, &page, &count, cross_count),
                    STORFS_OK);
  TEST_ASSERT_EQUAL(count, cross_count);

  // Verify all allocated pages across the boundary
  uint8_t alloc;
  for(storfs_page_t i = page; i < page + cross_count; i++) {
    TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, i, &alloc), STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_ALLOC);
  }

  // Free them and verify
  TEST_ASSERT_EQUAL(bitmap_free_contiguous(fs, page, &count, cross_count),
                    STORFS_OK);
  TEST_ASSERT_EQUAL(count, cross_count);

  for(storfs_page_t i = page; i < page + cross_count; i++) {
    TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, i, &alloc), STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_FREE);
  }
}
