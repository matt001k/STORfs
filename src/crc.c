#include "crc.h"

#include "core.h"
#include "storfs.h"

storfs_err_t crc_compare(storfs_t            *storfsInst,
                         storfs_file_header_t storfsInfo,
                         const uint8_t       *buf,
                         uint32_t             bufLen) {
  if(storfsInfo.crc == (STORFS_CRC_CALC(storfsInst, buf, bufLen))) {
    STORFS_LOGD(TAG, "CRC Code Correct");
    return STORFS_OK;
  }

  STORFS_LOGE(TAG, "CRC Code Returned Incorrectly");
  return STORFS_ERR_CRC_MISMATCH;
}

storfs_err_t crc_header_check(storfs_t *storfsInst, storfs_loc_t storfsLoc) {
  storfs_file_header_t storfsInfo;
  uint32_t             strLen = 0;

  file_header_store_helper(storfsInst,
                           &storfsInfo,
                           storfsLoc,
                           "CRC Header Check");

  while(storfsInfo.fileName[strLen++] != '\0')
    ;

  return crc_compare(storfsInst,
                     storfsInfo,
                     (uint8_t *)storfsInfo.fileName,
                     strLen);
}

storfs_err_t
crc_file_check(storfs_t *storfsInst, storfs_loc_t storfsLoc, uint32_t len) {
  storfs_file_header_t storfsInfo;
  uint32_t             headerLen = STORFS_HEADER_TOTAL_SIZE;
  uint8_t              buf[len];

  file_header_store_helper(storfsInst,
                           &storfsInfo,
                           storfsLoc,
                           "CRC File Check");
  if((storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == 0) {
    headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
  }

  if(storfsInst->read(storfsInst, storfsLoc.pageLoc, headerLen, buf, len) !=
     STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }

  return crc_compare(storfsInst, storfsInfo, buf, len);
}
