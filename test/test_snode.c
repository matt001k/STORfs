#include "fake_flash.h"
#include "helper_randomizer.h"
#include "mock_crc.h"
#include "snode.h"
#include "unity.h"

#include <stddef.h>
#include <string.h>

#define SIZE_SNODE_COMPARE fake_storfs_get_page_size()
#define NUM_SNODE_COMPARE  (SIZE_SNODE_COMPARE / sizeof(SNode))
#define FAKE_CRC16         (0x123A)

static storfs_t *fs;

void setUp(void) {
  fs = fake_storfs_init();
}

void tearDown(void) {}

void test_snode_create(void) {
  storfs_page_t flash_page = fake_storfs_get_page_count() / 2;
  SNode         snode      = { 0 };

  // Create nodes until filled up page
  for(size_t i = 0; i < fake_storfs_get_page_size(); i += sizeof(SNode)) {
    storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);
    TEST_ASSERT(snode_create(fs, flash_page, i, snode) == STORFS_OK);
  }

  // Test creating a node out of bounds
  TEST_ASSERT(
      snode_create(fs, flash_page, fake_storfs_get_page_size(), snode) !=
      STORFS_OK);

  // Test read failure
  fake_storfs_fail_op(READ, true);
  TEST_ASSERT(snode_create(fs, flash_page, 0, snode) == STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false);

  // Test write failure
  fake_storfs_fail_op(WRITE, true);
  storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);
  TEST_ASSERT(snode_create(fs, flash_page, 0, snode) ==
              STORFS_ERR_WRITE_FAILED);
  fake_storfs_fail_op(WRITE, false);

  // Test improper input
  TEST_ASSERT(snode_create(NULL, flash_page, 0, snode) ==
              STORFS_ERR_NULL_POINTER);
}

void test_snode_lookup(void) {
  storfs_page_t flash_page   = fake_storfs_get_page_count() / 2;
  SNode        *create_snode = (SNode *)random_array(SIZE_SNODE_COMPARE);

  // Create nodes until filled up page
  for(size_t i = 0; i < NUM_SNODE_COMPARE; i++) {
    create_snode[i].crc = 0;
    storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);
    TEST_ASSERT(
        snode_create(fs, flash_page, i * sizeof(SNode), create_snode[i]) ==
        STORFS_OK);
  }

  // Validate nodes
  SNode compare_snode;
  for(size_t i = 0; i < NUM_SNODE_COMPARE; i++) {
    storfs_crc16_ExpectAnyArgsAndReturn(FAKE_CRC16);
    TEST_ASSERT(
        snode_lookup(fs, flash_page, i * sizeof(SNode), &compare_snode) ==
        STORFS_OK);
    compare_snode.crc = 0;
    TEST_ASSERT(memcmp(&compare_snode, &create_snode[i], sizeof(SNode)) == 0);
  }
  random_array_free((uint8_t *)create_snode);

  // Test read failed
  fake_storfs_fail_op(READ, true);
  TEST_ASSERT(snode_lookup(fs, flash_page, 0, &compare_snode) ==
              STORFS_ERR_READ_FAILED);
  fake_storfs_fail_op(READ, false);

  // Test failed crc
  storfs_crc16_ExpectAnyArgsAndReturn(0);
  TEST_ASSERT(snode_lookup(fs, flash_page, 0, &compare_snode) ==
              STORFS_ERR_CRC_MISMATCH);

  // Test improper inputs
  TEST_ASSERT(snode_lookup(NULL, flash_page, 0, &compare_snode) ==
              STORFS_ERR_NULL_POINTER);
  TEST_ASSERT(snode_lookup(fs, flash_page, 0, NULL) == STORFS_ERR_NULL_POINTER);

  // Test read out of bounds
  TEST_ASSERT(snode_lookup(fs,
                           flash_page,
                           fake_storfs_get_page_size(),
                           &compare_snode) == STORFS_ERR_INVALID_PARAM);
  TEST_ASSERT(
      snode_lookup(fs, fake_storfs_get_page_count(), 0, &compare_snode) ==
      STORFS_ERR_INVALID_PARAM);
}
