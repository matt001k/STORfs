#include "snode.h"

#include "crc.h"

static inline storfs_err_t snode_check_read(storfs_t     *fs,
                                            storfs_page_t page,
                                            storfs_byte_t byte,
                                            uint8_t      *buf,
                                            uint32_t      size) {
  if(page >= fs->pageCount || byte + size > fs->pageSize) {
    return STORFS_ERR_INVALID_PARAM;
  }

  storfs_err_t err = fs->read(fs, page, byte, buf, size);

  if(err != STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }

  return STORFS_OK;
}

storfs_err_t
snode_create(storfs_t *fs, storfs_page_t page, storfs_byte_t byte, SNode node) {
  if(!fs) {
    return STORFS_ERR_NULL_POINTER;
  }

  if(byte + sizeof(SNode) > fs->pageSize) {
    return STORFS_ERR_INVALID_PARAM;
  }

  storfs_err_t err = snode_check_read(fs, page, 0, fs->buf, fs->pageSize);

  if(err != STORFS_OK) {
    return err;
  }

  node.crc          = 0;
  node.crc          = storfs_crc16((uint8_t *)&node, sizeof(SNode));
  SNode *write_node = (SNode *)&fs->buf[byte];
  *write_node       = node;

  err = fs->write(fs, page, 0, fs->buf, fs->pageSize);

  if(err != STORFS_OK) {
    err = STORFS_ERR_WRITE_FAILED;
  }

  return err;
}

storfs_err_t snode_lookup(storfs_t     *fs,
                          storfs_page_t page,
                          storfs_byte_t byte,
                          SNode        *node) {
  if(!fs || !node) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_err_t err =
      snode_check_read(fs, page, byte, (uint8_t *)node, sizeof(SNode));

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
