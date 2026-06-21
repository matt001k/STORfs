#include "atomic.h"
#include "bitmap.h"
#include "common.h"
#include "fake_flash.h"
#include "helper_randomizer.h"
#include "mock_crc.h"
#include "snode.h"
#include "unity.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE_SNODE_COMPARE        fake_storfs_get_page_size()
#define NUM_SNODE_COMPARE         (SIZE_SNODE_COMPARE / sizeof(SNode))
#define FAKE_CRC16                (0x123A)
#define FAKE_NAME                 "STORFS_FILE_TEST_"
#define MEMBER_SIZE(type, member) (sizeof(((type *)0)->member))
#define DIRECT_ARRAY_SIZE         (MEMBER_SIZE(SNode, direct) / sizeof(SNodeExtent))

#define DATA_PAGES_PER_INDIRECT_PAGE(f) ((f->pageSize / sizeof(SNodeExtent)))
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
  TEST_ASSERT_EQUAL(bitmap_create(fs), STORFS_OK);
}

void tearDown(void) {}

void test_snode_create(void) {
  SNode         snode                      = { 0 };
  char          name[STORFS_MAX_FILE_NAME] = { 0 };
  storfs_page_t page;

  // Create nodes until filled up filesystem
  storfs_page_t data_begin = fs->bitmap.hint;
  for(size_t i = data_begin; i < fs->pageCount; i++) {
    page = i;

    storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

    snprintf(name, STORFS_MAX_FILE_NAME, FAKE_NAME "%lu", i);

    storfs_page_t read_page;
    TEST_ASSERT_EQUAL(snode_create(fs, name, &read_page, SNODE_TYPE_FILE),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(read_page, page);
  }

  // Test creating a node out of bounds
  page++;
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_NO_FREE_BLOCKS);

  // Free all pages
  for(size_t i = data_begin; i < fs->pageCount + 1000; i++) {
    bitmap_alloc_page(fs, i, PAGE_FREE);
  }

  // Test read failure
  fake_storfs_fail_op(READ, true, 1);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);

  // Test write failure
  fake_storfs_fail_op(WRITE, true, 1);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false, 0);
  fake_storfs_fail_op(WRITE, true, 2);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false, 0);
  fake_storfs_fail_op(WRITE, true, 3);
  storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false, 0);

  // Test improper input
  TEST_ASSERT_EQUAL(snode_create(NULL, name, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_create(fs, NULL, &page, SNODE_TYPE_FILE),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_create(fs, name, NULL, SNODE_TYPE_FILE),
                    STORFS_ERR_NULL_POINTER);
}

void test_snode_write(void) {
  const uint32_t buf_size  = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t       *write_buf = random_array(buf_size);
  uint8_t       *read_buf  = (uint8_t *)calloc(buf_size, sizeof(uint8_t));
  char           name[STORFS_MAX_FILE_NAME] = FAKE_NAME "123";
  storfs_page_t  page                       = 17;
  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  SNodeInst inst = { 0 };
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  // Write all the data from the file, read it and compare
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, buf_size),
                    STORFS_OK);
  TEST_ASSERT_EQUAL(snode_read_data(fs, &inst, read_buf, buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(memcmp(write_buf, read_buf, buf_size), 0);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.node.size, 0);
  free(write_buf);
  free(read_buf);
}

// void test_snode_write_read(void) {
//   const uint32_t buf_size  = MULTIPLE_INDIRECT_DATA_SIZE(fs);
//   uint8_t       *write_buf = random_array(buf_size);
//   uint8_t       *read_buf  = (uint8_t *)calloc(buf_size, sizeof(uint8_t));
//   storfs_page_t  page      = 17;
//   char           name[STORFS_MAX_FILE_NAME] = FAKE_NAME "123";
//
//   storfs_crc16_IgnoreAndReturn(FAKE_CRC16);
//
//   TEST_ASSERT_EQUAL(snode_create(fs, name, &page), STORFS_OK);
//
//   // Write all the data from the file, read it and compare
//   TEST_ASSERT_EQUAL(snode_write_data(fs, page, write_buf, buf_size),
//   STORFS_OK); TEST_ASSERT_EQUAL(snode_read_data(fs, page, 0, read_buf,
//   buf_size),
//                     STORFS_OK);
//   TEST_ASSERT_EQUAL(memcmp(write_buf, read_buf, buf_size), 0);
//
//   // Chunk read it to check offset paramter
//   uint32_t chunk_size = fs->pageSize * 3;
//   for(uint32_t i = 0; i < buf_size; i += chunk_size) {
//     uint32_t data_remain = buf_size - i;
//     uint32_t data_size   = MIN(data_remain, chunk_size);
//     TEST_ASSERT_EQUAL(snode_read_data(fs, page, i, read_buf, data_size),
//                       STORFS_OK);
//     TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
//   }
//
//   // Test reading past boundaries
//   TEST_ASSERT_EQUAL(snode_read_data(fs, page, buf_size, read_buf, 1),
//                     STORFS_ERR_INVALID_PARAM);
//
//   random_array_free(write_buf);
//   free(read_buf);
// }
//

void test_snode_find_location(void) {
  const uint32_t buf_size                   = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t       *write_buf                  = random_array(buf_size);
  storfs_page_t  page                       = 17;
  char           name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst      inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, buf_size),
                    STORFS_OK);

  // Test obtaining the location now, will be in indirect extents which has yet
  // to be created
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_ERR_NOT_FOUND);

  // Erase and then fill the SNode all the way up
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.node.size, 0);

  // Fill up an snode to the maximum size of data
  uint32_t chunk_size = 1024;
  uint8_t *read_buf   = (uint8_t *)calloc(chunk_size, sizeof(uint8_t));
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t data_remain = buf_size - i;
    uint32_t data_size   = MIN(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, &write_buf[i], data_size),
                      STORFS_OK);

    // Test obtaining data from the last written location
    data_remain -= data_size;
    if(data_remain) {
      TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, i), STORFS_OK);
      TEST_ASSERT_EQUAL(snode_read_data(fs, &inst, read_buf, data_size),
                        STORFS_OK);
      TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
    }
  }

  // Test obtaining the location now, will be at the very end of the SNode
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_ERR_NO_SPACE);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, buf_size),
                    STORFS_ERR_NO_SPACE);

  // Test getting the data almost at the end of the file
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, buf_size - 1),
                    STORFS_OK);

  random_array_free(write_buf);
  free(read_buf);

  // Test improper parameters
  TEST_ASSERT_EQUAL(snode_find_write_location(NULL, &inst),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, NULL),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_find_read_location(NULL, &inst, 0),
                    STORFS_ERR_NULL_POINTER);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, NULL, 0),
                    STORFS_ERR_NULL_POINTER);
}

void test_snode_write_read_alternate(void) {
  const uint32_t buf_size     = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t       *write_buf    = random_array(buf_size);
  uint8_t       *read_buf     = (uint8_t *)calloc(buf_size, sizeof(uint8_t));
  storfs_page_t  snode_1_page = 17;
  storfs_page_t  snode_2_page = snode_1_page++;
  char           snode_1_name[STORFS_MAX_FILE_NAME] = FAKE_NAME "snode_1";
  char           snode_2_name[STORFS_MAX_FILE_NAME] = FAKE_NAME "snode_1";

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(
      snode_create(fs, snode_1_name, &snode_1_page, SNODE_TYPE_FILE),
      STORFS_OK);
  TEST_ASSERT_EQUAL(
      snode_create(fs, snode_2_name, &snode_2_page, SNODE_TYPE_FILE),
      STORFS_OK);

  // Write all the data from the file, read it and compare
  SNodeInst inst_1 = { 0 };
  SNodeInst inst_2 = { 0 };
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst_1), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst_1, 0), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst_2), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst_2, 0), STORFS_OK);

  // Chunk read it to check offset paramter
  uint32_t chunk_size = 693;
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t data_remain = buf_size - i;
    uint32_t data_size   = MIN(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(snode_write_data(fs, &inst_1, &write_buf[i], data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(snode_read_data(fs, &inst_1, read_buf, data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
    TEST_ASSERT_EQUAL(snode_write_data(fs, &inst_2, &write_buf[i], data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(snode_read_data(fs, &inst_2, read_buf, data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
  }
  TEST_ASSERT_EQUAL(inst_1.node.size, buf_size);
  TEST_ASSERT_EQUAL(inst_2.node.size, buf_size);

  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst_1, buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst_1.node.size, 0);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst_2, buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst_2.node.size, 0);

  // TODO test read here

  random_array_free(write_buf);
  free(read_buf);
}
