#include "bitmap.h"
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
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_ERR_NO_SPACE);

  // Test invalid args
  TEST_ASSERT_EQUAL(bitmap_alloc(NULL, &page), STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, NULL), STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_find_next(NULL, &page), STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(bitmap_find_next(fs, NULL), STORFS_ERR_NULL_POINTER);

  // Test failure to read
  fake_storfs_fail_op(READ, true);
  TEST_ASSERT_EQUAL(bitmap_alloc(fs, &page), STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false);
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
  TEST_ASSERT_EQUAL(bitmap_free(NULL, hint_original), STORFS_ERR_NULL_POINTER);

  // Test freeing all allocated pages
  uint8_t alloc;
  for(uint32_t i = pages_to_fill - 1; i >= hint_original; i--) {
    storfs_err_t err = bitmap_get_alloc(fs, i, &alloc);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
    TEST_ASSERT_EQUAL(alloc, PAGE_ALLOC);
    err = bitmap_free(fs, i);
    TEST_ASSERT_EQUAL(err, STORFS_OK);
  }

  // Test failure to read
  fake_storfs_fail_op(READ, true);
  TEST_ASSERT_EQUAL(bitmap_free(fs, hint_original), STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false);
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

  // Test failure to read
  fake_storfs_fail_op(READ, true);
  TEST_ASSERT_EQUAL(bitmap_get_alloc(fs, hint_original, &alloc),
                    STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false);
}

void test_bitmap_create(void) {
  // Test invalid parameters
  TEST_ASSERT_EQUAL(bitmap_create(NULL), STORFS_ERR_NULL_POINTER);

  // Test invalid write operation
  fake_storfs_fail_op(WRITE, true);
  TEST_ASSERT_EQUAL(bitmap_create(fs), STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false);

  // Test invalid erase operation
  fake_storfs_fail_op(ERASE, true);
  TEST_ASSERT_EQUAL(bitmap_create(fs), STORFS_ERR_ERASE_FAILED);
  fake_storfs_fail_op(ERASE, false);
}
