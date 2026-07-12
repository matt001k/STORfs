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

#define RANDOM_DATA_MAX_CHUNK (8192)

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

static inline storfs_err_t get_err_compare(uint32_t      data_remain,
                                           storfs_size_t chunk_size) {
  storfs_err_t compare = STORFS_ERR_END_OF_FILE;
  if(data_remain > chunk_size) {
    compare = STORFS_OK;
  }

  return compare;
}

static inline uint32_t calc_max_idx(void) {
#define EXTENTS_PER_PAGE(f) (((f)->pageSize / sizeof(SNodeExtent)))
  const uint32_t epp     = EXTENTS_PER_PAGE(fs);
  const uint32_t si_size = DIRECT_EXTENT_SIZE + epp;
  return si_size + epp * epp;
}

/*!
 @brief Simulate IDX Advancement

 @details Mirrors how get_modify_extents/snode_read_or_write_data pack bytes
          into extents, a fresh extent is sized to exactly cover whatever is
          left of the current write call (rounded up to whole pages), so
          write.idx only advances once that extent's page-aligned capacity is
          fully consumed. Chunk sizes that aren't a multiple of the page size
          leave a partial page of slack that carries over into the next write
          call.

 @param data_size data to advance
 @param extent_remaining the remainder of the extent before moving to the next
                         index

 @return The change in index
 */
static uint32_t simulate_idx_advance(uint32_t  data_size,
                                     uint32_t *extent_remaining) {
  uint32_t idx_delta = 0;
  while(data_size > 0) {
    if(!*extent_remaining) {
      *extent_remaining = CEIL_DIV(data_size, fs->pageSize) * fs->pageSize;
    }
    uint32_t consumed = MIN(data_size, *extent_remaining);
    *extent_remaining -= consumed;
    data_size -= consumed;
    if(!*extent_remaining) {
      idx_delta++;
    }
  }
  return idx_delta;
}

static uint8_t *
fill_snode(SNodeInst *inst, storfs_size_t *buf_size, storfs_size_t chunk_size) {
  uint32_t max = calc_max_idx();

  *buf_size         = chunk_size * max;
  uint32_t leftover = fs->pageSize - sizeof(SNode);
  *buf_size += leftover;
  uint8_t *buf = random_array(*buf_size);

  TEST_ASSERT_EQUAL(snode_write_data(fs, inst, buf, &leftover), STORFS_OK);
  TEST_ASSERT_EQUAL(inst->write.idx, 1);

  uint32_t extent_remaining = 0;
  uint32_t write_idx        = 1;
  uint32_t i;
  for(i = leftover; i < *buf_size; i += chunk_size) {
    uint32_t     data_remain = *buf_size - i;
    uint32_t     data_size   = MIN(data_remain, chunk_size);
    storfs_err_t err         = snode_write_data(fs, inst, &buf[i], &data_size);
    write_idx +=
        simulate_idx_advance(MIN(data_remain, chunk_size), &extent_remaining);
    TEST_ASSERT_EQUAL(inst->write.idx, write_idx);
    storfs_err_t compare = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, compare);
  }

  if(i < *buf_size) {
    uint32_t data_size = *buf_size - i;
    TEST_ASSERT_EQUAL(snode_write_data(fs, inst, &buf[i], &data_size),
                      STORFS_ERR_END_OF_FILE);
  }

  return buf;
}

void test_snode_write(void) {
  uint32_t      buf_size  = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t      *write_buf = random_array(buf_size);
  uint8_t      *read_buf  = (uint8_t *)calloc(buf_size, sizeof(uint8_t));
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME "123";
  storfs_page_t page                       = 17;
  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  SNodeInst inst = { 0 };
  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  // Write all the data from the file, read it and compare
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  uint32_t bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, &bytes), STORFS_OK);
  TEST_ASSERT_EQUAL(bytes, buf_size);
  TEST_ASSERT_EQUAL(inst.node.size, buf_size);
  bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_read_data(fs, &inst, read_buf, &bytes), STORFS_OK);
  TEST_ASSERT_EQUAL(bytes, buf_size);
  TEST_ASSERT_EQUAL(memcmp(write_buf, read_buf, buf_size), 0);
  bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &bytes), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.node.size, 0);
  random_array_free(write_buf);

  write_buf = fill_snode(&inst, &buf_size, fs->pageSize);

  // Test writing beyond the bounds of the SNode
  bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, &bytes),
                    STORFS_ERR_NO_FREE_BLOCKS);

  random_array_free(write_buf);
  free(read_buf);
}

void test_snode_write_read(void) {
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME "123";
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  storfs_size_t chunk_size = fs->pageSize;
  uint32_t      buf_size;
  uint8_t      *write_buf;
  write_buf = fill_snode(&inst, &buf_size, chunk_size);

  // Chunk read it to check offset paramter
  chunk_size        = fs->pageSize * 3;
  uint8_t *read_buf = (uint8_t *)calloc(chunk_size, sizeof(uint8_t));
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t     data_remain = buf_size - i;
    uint32_t     data_size   = MIN(data_remain, chunk_size);
    uint32_t     data_read   = data_size;
    storfs_err_t err         = snode_read_data(fs, &inst, read_buf, &data_read);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_read), 0);
    storfs_err_t compare = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, compare);
  }

  // Test reading past boundaries
  uint32_t data_read = chunk_size;
  TEST_ASSERT_EQUAL(snode_read_data(fs, &inst, read_buf, &data_read),
                    STORFS_ERR_NO_FREE_BLOCKS);

  random_array_free(write_buf);
  free(read_buf);
}

void test_snode_find_location(void) {
  uint32_t      buf_size                   = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t      *write_buf                  = random_array(buf_size);
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  uint32_t bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, &bytes), STORFS_OK);
  TEST_ASSERT_EQUAL(bytes, buf_size);

  // Test obtaining the location now, will be in indirect extents which has yet
  // to be created
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_ERR_NOT_FOUND);

  // Erase and then fill the SNode all the way up
  bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(bytes, buf_size);
  TEST_ASSERT_EQUAL(inst.node.size, 0);

  random_array_free(write_buf);
  storfs_size_t chunk_size = fs->pageSize;
  write_buf                = fill_snode(&inst, &buf_size, chunk_size);

  // Now compare after filling
  uint8_t *read_buf = (uint8_t *)calloc(buf_size, sizeof(uint8_t));
  bytes             = buf_size;
  TEST_ASSERT_EQUAL(snode_read_data(fs, &inst, read_buf, &buf_size),
                    STORFS_ERR_END_OF_FILE);
  TEST_ASSERT_EQUAL(bytes, buf_size);
  TEST_ASSERT_EQUAL(memcmp(write_buf, read_buf, buf_size), 0);
  free(read_buf);

  // Test obtaining the location now, will be at the very end of the SNode
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_ERR_NO_SPACE);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, buf_size),
                    STORFS_ERR_NO_SPACE);

  // Test getting the data almost at the end of the file
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, buf_size - 1),
                    STORFS_OK);

  bytes = buf_size;
  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, &bytes),
                    STORFS_ERR_NO_FREE_BLOCKS);
  random_array_free(write_buf);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.node.size, 0);
  TEST_ASSERT_EQUAL(inst.write.idx, 0);

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

static void full_read_helper(SNodeInst    *inst,
                             uint8_t      *write_buf,
                             storfs_size_t file_size,
                             storfs_size_t read_offset,
                             bool          random,
                             storfs_size_t chunk_read) {
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, inst, 0), STORFS_OK);
  storfs_size_t offset = 0;
  storfs_err_t  err;
  storfs_size_t chunk_size;
  do {
    if(random) {
      chunk_size = random_integer(RANDOM_DATA_MAX_CHUNK);
    } else {
      chunk_size = chunk_read;
    }

    uint8_t      *read_buf = (uint8_t *)malloc(chunk_size);
    storfs_size_t read     = chunk_size;
    err                    = snode_read_data(fs, inst, read_buf, &read);
    if(err == STORFS_OK) {
      TEST_ASSERT_EQUAL(read, chunk_size);
    } else {
      TEST_ASSERT_EQUAL(err, STORFS_ERR_END_OF_FILE);
    }
    TEST_ASSERT_EQUAL(memcmp(&write_buf[offset], read_buf, read), 0);
    offset += read;
    free(read_buf);
  } while(err == STORFS_OK);
}

void test_snode_misaligned_read(void) {
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);

  storfs_size_t chunk_size = fs->pageSize * 6;
  uint32_t      buf_size;
  uint8_t      *write_buf;
  write_buf = fill_snode(&inst, &buf_size, chunk_size);

  uint32_t read_offset = fs->pageSize * 2;
  uint32_t read_size   = fs->pageSize * 3;
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, read_offset),
                    STORFS_OK);
  full_read_helper(&inst, write_buf, buf_size, read_offset, false, read_size);

  random_array_free(write_buf);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.write.idx, 0);
}

static void
simple_read_helper(SNodeInst *inst, uint8_t *write_buf, uint32_t write_size) {
  // Read all data in one go
  uint8_t *read_buf = (uint8_t *)calloc(write_size, sizeof(uint8_t));
  uint32_t bytes    = write_size;
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, inst, 0), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_read_data(fs, inst, read_buf, &bytes),
                    STORFS_ERR_END_OF_FILE);
  TEST_ASSERT_EQUAL(memcmp(write_buf, read_buf, write_size), 0);
  free(read_buf);
}

void test_read_after_full_write(void) {
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  storfs_size_t chunk_size = fs->pageSize;
  uint32_t      buf_size;
  uint8_t      *write_buf;
  write_buf = fill_snode(&inst, &buf_size, chunk_size);

  // Read all of the data to make sure it matches
  uint8_t *read_buf = (uint8_t *)calloc(chunk_size, sizeof(uint8_t));
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t     data_remain = buf_size - i;
    uint32_t     data_size   = MIN(data_remain, chunk_size);
    storfs_err_t err         = snode_read_data(fs, &inst, read_buf, &data_size);
    storfs_err_t compare     = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, compare);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
  }

  random_array_free(write_buf);
  free(read_buf);
}

void test_read_after_full_write_update_cache(void) {
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  storfs_size_t chunk_size = fs->pageSize;
  uint32_t      buf_size;
  uint8_t      *write_buf;
  write_buf = fill_snode(&inst, &buf_size, chunk_size);

  // Read all of the data to make sure it matches
  uint8_t *read_buf = (uint8_t *)calloc(chunk_size, sizeof(uint8_t));
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t data_remain = buf_size - i;
    uint32_t data_size   = MIN(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, i), STORFS_OK);
    storfs_err_t err     = snode_read_data(fs, &inst, read_buf, &data_size);
    storfs_err_t compare = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, compare);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
  }
  free(read_buf);

  simple_read_helper(&inst, write_buf, buf_size);

  random_array_free(write_buf);
}

void test_read_after_random_page_full_write(void) {
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  storfs_size_t chunk_size = fs->pageSize * 5;
  uint32_t      buf_size;
  uint8_t      *write_buf;
  write_buf = fill_snode(&inst, &buf_size, chunk_size);
  simple_read_helper(&inst, write_buf, buf_size);
  random_array_free(write_buf);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.write.idx, 0);

  chunk_size = fs->pageSize * 12;
  write_buf  = fill_snode(&inst, &buf_size, chunk_size);
  simple_read_helper(&inst, write_buf, buf_size);
  random_array_free(write_buf);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.write.idx, 0);

  // Offset chunk not exactly on a byte boundary
  chunk_size = (storfs_size_t)((float)fs->pageSize * 5.25);
  write_buf  = fill_snode(&inst, &buf_size, chunk_size);
  simple_read_helper(&inst, write_buf, buf_size);
  random_array_free(write_buf);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.write.idx, 0);

  // Write random chunk size to the file until it filles up
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  write_buf              = NULL;
  uint32_t     allocated = 0;
  storfs_err_t err;
  uint32_t     file_size = 0;
  do {
    chunk_size            = random_integer(RANDOM_DATA_MAX_CHUNK);
    uint32_t      written = chunk_size;
    storfs_size_t offset  = allocated;

    allocated += chunk_size;
    write_buf = realloc(write_buf, allocated);

    err = snode_write_data(fs, &inst, &write_buf[offset], &written);
    if(err == STORFS_OK) {
      TEST_ASSERT_EQUAL(written, chunk_size);
    } else {
      TEST_ASSERT_EQUAL(err, STORFS_ERR_END_OF_FILE);
      // The total file size is equal to the last data written
      file_size = allocated - (chunk_size - written);
    }
  } while(err == STORFS_OK);
  simple_read_helper(&inst, write_buf, file_size);

  // Read back random chunk size
  full_read_helper(&inst, write_buf, file_size, 0, true, 0);
  random_array_free(write_buf);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst, &file_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst.write.idx, 0);
}

void test_fill_stagger(void) {
  uint32_t      buf_size                   = MULTIPLE_INDIRECT_DATA_SIZE(fs);
  uint8_t      *write_buf                  = random_array(buf_size);
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  storfs_size_t chunk_size = fs->pageSize;
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t data_remain = buf_size - i;
    uint32_t data_size   = MIN(data_remain, chunk_size);
    uint32_t idx_compare = i / chunk_size + 1;

    storfs_err_t err = snode_write_data(fs, &inst, &write_buf[i], &data_size);
    storfs_err_t err_compare = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, err_compare);
    TEST_ASSERT_EQUAL(inst.write.idx, idx_compare);
    // Test that finding the location is also correct
    snode_find_write_location(fs, &inst);
    TEST_ASSERT_EQUAL(inst.write.idx, idx_compare);
  }

  // Read all of the data to make sure it matches
  uint8_t *read_buf = (uint8_t *)calloc(chunk_size, sizeof(uint8_t));
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t     data_remain = buf_size - i;
    uint32_t     data_size   = MIN(data_remain, chunk_size);
    storfs_err_t err         = snode_read_data(fs, &inst, read_buf, &data_size);
    storfs_err_t compare     = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, compare);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
  }

  random_array_free(write_buf);
  free(read_buf);
}

void test_fill_boundary(void) {
  storfs_page_t page                       = 17;
  char          name[STORFS_MAX_FILE_NAME] = FAKE_NAME;
  SNodeInst     inst                       = { 0 };

  storfs_crc16_IgnoreAndReturn(FAKE_CRC16);

  TEST_ASSERT_EQUAL(snode_create(fs, name, &page, SNODE_TYPE_FILE), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, page, &inst), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst, 0), STORFS_OK);

  // Test writing on boundary
  storfs_size_t chunk_size = fs->pageSize;
  uint32_t      buf_size   = chunk_size * calc_max_idx();
  uint32_t      leftover   = fs->pageSize - sizeof(SNode);
  buf_size += leftover;
  uint8_t *write_buf = random_array(buf_size);

  TEST_ASSERT_EQUAL(snode_write_data(fs, &inst, write_buf, &leftover),
                    STORFS_OK);

  for(uint32_t i = leftover; i < buf_size; i += chunk_size) {
    uint32_t data_remain = buf_size - i;
    uint32_t data_size   = chunk_size;

    storfs_err_t err = snode_write_data(fs, &inst, &write_buf[i], &data_size);
    storfs_err_t err_compare = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, err_compare);

    const uint32_t idx_compare = i / chunk_size + 2;
    TEST_ASSERT_EQUAL(inst.write.idx, idx_compare);

    // Test obtaining write location works as expected as well
    snode_find_write_location(fs, &inst);
    TEST_ASSERT_EQUAL(inst.write.idx, idx_compare);
  }

  // Read all of the data to make sure it matches
  uint8_t *read_buf = (uint8_t *)calloc(chunk_size, sizeof(uint8_t));
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t     data_remain = buf_size - i;
    uint32_t     data_size   = MIN(data_remain, chunk_size);
    storfs_err_t err         = snode_read_data(fs, &inst, read_buf, &data_size);
    storfs_err_t compare     = get_err_compare(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(err, compare);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
  }

  random_array_free(write_buf);
  free(read_buf);
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
  TEST_ASSERT_EQUAL(snode_lookup(fs, snode_1_page, &inst_1), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_lookup(fs, snode_2_page, &inst_2), STORFS_OK);

  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst_1), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst_1, 0), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_write_location(fs, &inst_2), STORFS_OK);
  TEST_ASSERT_EQUAL(snode_find_read_location(fs, &inst_2, 0), STORFS_OK);

  // Chunk read it to check offset paramter
  uint32_t chunk_size = 693;
  for(uint32_t i = 0; i < buf_size; i += chunk_size) {
    uint32_t data_remain = buf_size - i;
    uint32_t data_size   = MIN(data_remain, chunk_size);
    TEST_ASSERT_EQUAL(snode_write_data(fs, &inst_1, &write_buf[i], &data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(snode_read_data(fs, &inst_1, read_buf, &data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
    TEST_ASSERT_EQUAL(snode_write_data(fs, &inst_2, &write_buf[i], &data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(snode_read_data(fs, &inst_2, read_buf, &data_size),
                      STORFS_OK);
    TEST_ASSERT_EQUAL(memcmp(&write_buf[i], read_buf, data_size), 0);
  }
  TEST_ASSERT_EQUAL(inst_1.node.size, buf_size);
  TEST_ASSERT_EQUAL(inst_2.node.size, buf_size);

  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst_1, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst_1.node.size, 0);
  TEST_ASSERT_EQUAL(snode_erase_data(fs, &inst_2, &buf_size), STORFS_OK);
  TEST_ASSERT_EQUAL(inst_2.node.size, 0);

  random_array_free(write_buf);
  free(read_buf);
}
