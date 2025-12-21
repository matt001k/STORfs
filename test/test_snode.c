#include "atomic.h"
#include "core.h"
#include "fake_flash.h"
#include "helper_randomizer.h"
#include "mock_bitmap.h"
#include "mock_crc.h"
#include "snode.h"
#include "unity.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE_SNODE_COMPARE        fake_storfs_get_page_size()
#define NUM_SNODE_COMPARE         (SIZE_SNODE_COMPARE / sizeof(SNode))
#define FAKE_CRC16                (0x123A)
#define FAKE_NAME                 "STORFS_FILE_TEST_"
#define MEMBER_SIZE(type, member) (sizeof(((type *)0)->member))
#define DIRECT_ARRAY_SIZE         (MEMBER_SIZE(SNode, direct) / sizeof(uint32_t))

#define DATA_PAGES_PER_INDIRECT_PAGE(f) ((f->pageSize / sizeof(uint32_t)))
#define SINGLE_INDIRECT_DATA_CALC(f)                                           \
  (f->pageSize * DATA_PAGES_PER_INDIRECT_PAGE(f))

#define PAGE_DATA_SIZE(f) (f->pageSize - sizeof(SNode))
#define DIRECT_DATA_SIZE(f)                                                    \
  (PAGE_DATA_SIZE(f) + (f->pageSize * DIRECT_ARRAY_SIZE))
#define SINGLE_INDIRECT_DATA_SIZE(f)                                           \
  (DIRECT_DATA_SIZE(f) + SINGLE_INDIRECT_DATA_CALC(f))
#define MULTIPLE_INDIRECT_DATA_SIZE(f)                                         \
  (SINGLE_INDIRECT_DATA_SIZE(f) +                                              \
   (SINGLE_INDIRECT_DATA_CALC(f) * DATA_PAGES_PER_INDIRECT_PAGE(f)))

static storfs_t *fs;

void setUp(void) {
  fs = fake_storfs_init();
}

void tearDown(void) {}

void test_snode_create(void) {
  SNode         snode                      = { 0 };
  char          name[STORFS_MAX_FILE_NAME] = { 0 };
  storfs_page_t page;

  bitmap_alloc_IgnoreAndReturn(STORFS_OK);

  // Create nodes until filled up filesystem
  for(size_t i = 0; i < fs->pageCount; i++) {
    bitmap_find_next_ExpectAnyArgsAndReturn(STORFS_OK);
    page = i;
    bitmap_find_next_ReturnThruPtr_page(&page);

    storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);

    snprintf(name, STORFS_MAX_FILE_NAME, FAKE_NAME "%lu", i);

    storfs_page_t read_page;
    TEST_ASSERT_EQUAL(snode_create(fs, name, &read_page), STORFS_OK);
    TEST_ASSERT_EQUAL(read_page, page);
  }

  bitmap_find_next_ExpectAnyArgsAndReturn(STORFS_ERR_NO_FREE_BLOCKS);
  // Test creating a node out of bounds
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page), STORFS_ERR_NO_FREE_BLOCKS);

  bitmap_find_next_IgnoreAndReturn(STORFS_OK);
  bitmap_find_next_ReturnThruPtr_page(&page);

  // Test read failure
  fake_storfs_fail_op(READ, true);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page), STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false);

  // Test write failure
  fake_storfs_fail_op(WRITE, true);
  storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page), STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false);

  // Test improper input
  TEST_ASSERT_EQUAL(snode_create(NULL, name, &page), STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_create(fs, NULL, &page), STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_create(fs, name, NULL), STORFS_ERR_NULL_POINTER);
}

void test_snode_write_read(void) {
  const uint32_t buf_size  = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t       *write_buf = random_array(buf_size);
  uint8_t       *read_buf  = (uint8_t *)calloc(buf_size, sizeof(uint8_t));
  storfs_page_t  page      = 17;
  char           name[STORFS_MAX_FILE_NAME] = FAKE_NAME "123";

  // Calculate number of page allocations needed for mock
  uint32_t iter = 0;

  uint32_t snode_data_page_size = PAGE_DATA_SIZE(fs);
  uint32_t data_beyond_snode    = buf_size - snode_data_page_size;
  uint32_t data_pages_needed    = CEIL_DIV(data_beyond_snode, fs->pageSize);

  uint32_t direct_pages = (data_pages_needed < DIRECT_ARRAY_SIZE)
                              ? data_pages_needed
                              : DIRECT_ARRAY_SIZE;
  iter += direct_pages;

  if(data_pages_needed > DIRECT_ARRAY_SIZE) {
    // Single indirect block itself
    iter++;

    uint32_t single_indirect_data_pages = data_pages_needed - DIRECT_ARRAY_SIZE;
    uint32_t pages_in_single_indirect =
        (single_indirect_data_pages < DATA_PAGES_PER_INDIRECT_PAGE(fs))
            ? single_indirect_data_pages
            : DATA_PAGES_PER_INDIRECT_PAGE(fs);
    iter += pages_in_single_indirect;
  }

  // If we need multiple indirect
  if(data_pages_needed > DIRECT_ARRAY_SIZE + DATA_PAGES_PER_INDIRECT_PAGE(fs)) {
    // Multiple indirect block itself
    iter++;

    uint32_t pages_before_multiple =
        DIRECT_ARRAY_SIZE + DATA_PAGES_PER_INDIRECT_PAGE(fs);
    uint32_t remaining_pages = data_pages_needed - pages_before_multiple;

    // Single indirect blocks within multiple
    uint32_t single_indirect_blocks_needed =
        CEIL_DIV(remaining_pages, DATA_PAGES_PER_INDIRECT_PAGE(fs));
    iter += single_indirect_blocks_needed;
    iter += remaining_pages;
  }

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  bitmap_find_next_ExpectAnyArgsAndReturn(STORFS_OK);
  bitmap_find_next_ReturnThruPtr_page(&page);
  bitmap_alloc_ExpectAnyArgsAndReturn(STORFS_OK);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page), STORFS_OK);

  storfs_page_t page_alloc[iter];
  for(uint32_t i = 0; i < iter; i++) {
    bitmap_alloc_ExpectAnyArgsAndReturn(STORFS_OK);
    page_alloc[i] = page + i + 1;
    bitmap_alloc_ReturnThruPtr_page(&page_alloc[i]);
  }

  // Write all the data from the file, read it and compare
  TEST_ASSERT_EQUAL(snode_write_data(fs, page, write_buf, buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_read_data(fs, page, 0, read_buf, buf_size),
                    STORFS_OK);
  TEST_ASSERT_EQUAL(memcmp(write_buf, read_buf, buf_size), 0);

  // Chunk read it to check offset paramter
  uint32_t chunk_size = fs->pageSize * 3;
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t read_remain = buf_size - i;
    uint32_t read_size   = MIN(read_remain, chunk_size);
    TEST_ASSERT_EQUAL(snode_read_data(fs, page, i, read_buf, read_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, read_size), 0);
  }

  // Test writing past boundaries
  TEST_ASSERT_EQUAL(snode_write_data(fs, page, write_buf, 1),
                    STORFS_ERR_NO_SPACE);

  // Test reading past boundaries
  TEST_ASSERT_EQUAL(snode_read_data(fs, page, buf_size, read_buf, 1),
                    STORFS_ERR_INVALID_PARAM);

  random_array_free(write_buf);
  free(read_buf);
}
