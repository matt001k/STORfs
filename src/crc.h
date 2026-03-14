#ifndef __STORFS_CRC_H__
#define __STORFS_CRC_H__

#include "storfs.h"

uint32_t storfs_crc32(const uint8_t *buf, uint32_t len);
/** @brief Used to compare crc code from a file and a buffer */
#if STORFS_USE_CRC == 1
#define STORFS_CRC_CALC(storfsInst, buf, buflen)                               \
  (storfsInst->crc(storfsInst, buf, buflen))
#else
uint16_t storfs_crc16(const uint8_t *buf, uint32_t bufLen);
#define STORFS_CRC_CALC(storfsInst, buf, buflen) storfs_crc16(buf, buflen)
#endif
storfs_err_t crc_compare(storfs_t            *storfsInst,
                         storfs_file_header_t storfsInfo,
                         const uint8_t       *buf,
                         uint32_t             bufLen);
storfs_err_t crc_header_check(storfs_t *storfsInst, storfs_loc_t storfsLoc);
storfs_err_t
crc_file_check(storfs_t *storfsInst, storfs_loc_t storfsLoc, uint32_t len);

#endif
