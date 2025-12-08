#include "core.h"
#include "fake_flash.h"
#include "hash.h"
#include "mock_crc.h"
#include "storfs.h"
#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_CRC32 (0x123ABC)

static storfs_t *fs;

void setUp(void) {
  fs = fake_storfs_init();
}

void tearDown(void) {}

void test_hash_create(void) {
  // Create hash table covering the whole file system
  for(size_t i = 0; i < fake_storfs_get_page_count(); i++) {
    storfs_crc32_ExpectAnyArgsAndReturn(FAKE_CRC32);
    TEST_ASSERT(hash_create(fs, i) == STORFS_OK);
  }

  // Test writing past available memory
  storfs_crc32_ExpectAnyArgsAndReturn(FAKE_CRC32);
  TEST_ASSERT(hash_create(fs, fake_storfs_get_page_count()) != STORFS_OK);

  // Test improper arguments
  TEST_ASSERT(hash_create(NULL, 0) == STORFS_ERR_NULL_POINTER);
}

void test_hash_add(void) {
  storfs_page_t flash_page = fake_storfs_get_page_count() / 2;
  const char   *name       = "hello_world";

  Bucket bucket = {
    100,
    128,
    0,
  };

  storfs_crc32_IgnoreAndReturn(FAKE_CRC32);
  TEST_ASSERT(hash_create(fs, flash_page) == STORFS_OK);

  // Test adding a proper bucket
  TEST_ASSERT(hash_add(fs, flash_page, name, bucket) == STORFS_OK);

  // Test invalid arguments
  TEST_ASSERT(hash_add(NULL, flash_page, name, bucket) != STORFS_OK);
  TEST_ASSERT(hash_add(fs, flash_page, NULL, bucket) != STORFS_OK);

  // Test reading failure
  fake_storfs_fail_op(READ, true, 1);
  TEST_ASSERT(hash_add(fs, flash_page, name, bucket) == STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);

  // Test writing failure
  fake_storfs_fail_op(WRITE, true, 1);
  TEST_ASSERT(hash_add(fs, flash_page, name, bucket) ==
              STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false, 0);

  // Test invalid contents
  uint8_t buf[fs->pageSize];
  memset(buf, 0, fs->pageSize);
  fs->write(fs, flash_page, 0, buf, fs->pageSize);
  TEST_ASSERT(hash_add(fs, flash_page, name, bucket) == STORFS_ERR_NOT_FOUND);
}

void test_hash_lookup(void) {
  storfs_page_t flash_page = fake_storfs_get_page_count() / 2;
  const char   *names[]    = {
    "hello_world_0", "hello_world_1", "hello_world_2",
    "hello_world_3", "hello_world_4", "hello_world_5",
    "hello_world_6", "hello_world_7", "hello_world_8",
  };

  Bucket buckets[ARRAY_SIZE(names)] = {
    {   100,  138,  1 },
    {   110,  148,  2 },
    {   111,  159,  3 },
    {   114, 1210,  1 },
    {  1102, 1211, 10 },
    { 11044, 1212, 40 },
  };

  storfs_crc32_ExpectAnyArgsAndReturn(FAKE_CRC32);
  TEST_ASSERT(hash_create(fs, flash_page) == STORFS_OK);

  // Test adding a proper bucket
  for(size_t i = 0; i < ARRAY_SIZE(names); i++) {
    storfs_crc32_ExpectAnyArgsAndReturn(FAKE_CRC32);
    TEST_ASSERT(hash_add(fs, flash_page, names[i], buckets[i]) == STORFS_OK);
  }

  // Test reading buckets
  for(size_t i = 0; i < ARRAY_SIZE(names); i++) {
    Bucket bucket = { 0 };
    storfs_crc32_ExpectAnyArgsAndReturn(FAKE_CRC32);
    TEST_ASSERT(hash_lookup(fs, flash_page, names[i], &bucket) == STORFS_OK);
    TEST_ASSERT(bucket.page == buckets[i].page);
    TEST_ASSERT(memcmp(&bucket, &buckets[i], sizeof(Bucket)) == 0);
  }

  Bucket bucket = { 0 };

  // Test input failure
  TEST_ASSERT(hash_lookup(NULL, flash_page, names[0], &bucket) ==
              STORFS_ERR_NULL_POINTER);
  TEST_ASSERT(hash_lookup(fs, flash_page, names[0], NULL) ==
              STORFS_ERR_NULL_POINTER);
  TEST_ASSERT(hash_lookup(fs, flash_page, NULL, &bucket) ==
              STORFS_ERR_NULL_POINTER);

  // Test CRC failure
  storfs_crc32_ExpectAnyArgsAndReturn(0);
  TEST_ASSERT(hash_lookup(fs, flash_page, names[0], &bucket) ==
              STORFS_ERR_CRC_MISMATCH);

  // Test get info failure via read failure
  fake_storfs_fail_op(READ, true, 1);
  TEST_ASSERT(hash_lookup(fs, flash_page, names[0], &bucket) ==
              STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false, 0);
}
