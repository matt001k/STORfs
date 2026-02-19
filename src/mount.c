#include "core.h"
#include "crc.h"
#include "storfs.h"

storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName) {
  if (!storfsInst || !partName) {
    return STORFS_ERR_NULL_POINTER;
  }

  storfs_file_header_t firstPartInfo[2];
  storfs_size_t strLen = 0;

  STORFS_LOGI(TAG, "Mounting File System");

  // If the user defined region for the first root directory byte added to the
  // size of the header is larger than the size of a page, return an error
  if ((storfsInst->firstByteLoc + STORFS_HEADER_TOTAL_SIZE) >
      storfsInst->pageSize) {
    STORFS_LOGE(TAG, "The user defined starting byte and header size is larger "
                     "than the user defined page size");
    return STORFS_ERROR;
  }

  // Store the first partition location into the cache
  storfsInst->cachedInfo.rootLocation[0].pageLoc = storfsInst->firstPageLoc;
  storfsInst->cachedInfo.rootLocation[0].byteLoc = storfsInst->firstByteLoc;

  // The second root header will be a page ahead of the first root header
  storfsInst->cachedInfo.rootLocation[1].byteLoc = 0;
  storfsInst->cachedInfo.rootLocation[1].pageLoc =
      storfsInst->cachedInfo.rootLocation[0].pageLoc + 1;

  // Store the header written to the storage and verify that the crc is correct
  // TODO
  file_header_store_helper(storfsInst, &firstPartInfo[0],
                           storfsInst->cachedInfo.rootLocation[0], "Root");
  file_info_display_helper(firstPartInfo[0]);
  file_header_store_helper(storfsInst, &firstPartInfo[1],
                           storfsInst->cachedInfo.rootLocation[1], "Root");
  file_info_display_helper(firstPartInfo[1]);

  // If file is empty create the root partition within the user defined
  // parameters The system will use two headers for the root
  if (((firstPartInfo[0].fileInfo & STORFS_INFO_REG_BLOCK_SIGN_EMPTY) ==
       0x60) ||
      ((firstPartInfo[1].fileInfo & STORFS_INFO_REG_BLOCK_SIGN_EMPTY) ==
       0x60)) {
    // Ensure that both of the roots are cleared
    if (storfsInst->erase(storfsInst,
                          storfsInst->cachedInfo.rootLocation[0].pageLoc) !=
        STORFS_OK) {
      return STORFS_ERROR;
    }
    if (storfsInst->erase(storfsInst,
                          storfsInst->cachedInfo.rootLocation[1].pageLoc) !=
        STORFS_OK) {
      return STORFS_ERROR;
    }
    // Set next open byte
    storfsInst->cachedInfo.nextOpenByte =
        ((storfsInst->cachedInfo.rootLocation[1].pageLoc + 1) *
         storfsInst->pageSize);

    // Get string length
    while (partName[strLen++] != '\0')
      ;

    // Error checking
    if (strLen == 0 || (storfsInst->cachedInfo.nextOpenByte >=
                        (storfsInst->pageCount * storfsInst->pageSize))) {
      STORFS_LOGE(TAG, "STORfs cannot be mounted");
      return STORFS_ERROR;
    }

    // Store file parameters
    for (storfs_size_t i = 0; i < strLen; i++) {
      firstPartInfo[0].fileName[i] = partName[i];
    }
    firstPartInfo[0].fileInfo =
        STORFS_INFO_REG_BLOCK_SIGN_PART_FULL | STORFS_INFO_REG_FILE_TYPE_ROOT;
    firstPartInfo[0].childLocation = storfsInst->cachedInfo.nextOpenByte;
    firstPartInfo[0].siblingLocation = 0x0;
    firstPartInfo[0].reserved = 0xFFFF;
    firstPartInfo[0].fragmentLocation = storfsInst->cachedInfo.nextOpenByte;
    firstPartInfo[0].fileSize = STORFS_HEADER_TOTAL_SIZE * 2;
    firstPartInfo[0].crc =
        STORFS_CRC_CALC(storfsInst, firstPartInfo[0].fileName, strLen);
    firstPartInfo[1] = firstPartInfo[0];

    // Write data to first available memory location defined by user
    if (file_header_create_helper(storfsInst, &firstPartInfo[0],
                                  storfsInst->cachedInfo.rootLocation[0],
                                  "Root") != STORFS_OK) {
      STORFS_LOGE(
          TAG, "The filesystem could not be created at location %ld%ld, %ld",
          (uint32_t)(storfsInst->cachedInfo.rootLocation[0].pageLoc >> 32),
          (uint32_t)(storfsInst->cachedInfo.rootLocation[0].pageLoc),
          storfsInst->cachedInfo.rootLocation[0].byteLoc);
      return STORFS_ERROR;
    }

    file_header_store_helper(storfsInst, &firstPartInfo[0],
                             storfsInst->cachedInfo.rootLocation[0], "Root");
    file_info_display_helper(firstPartInfo[0]);

    // Compare the CRC obtained from the file to the computed crc of the
    // filename
    if (crc_compare(storfsInst, firstPartInfo[0], firstPartInfo[0].fileName,
                    strLen) != STORFS_OK) {
      return STORFS_ERROR;
    }

    // Write data to second available memory location following the first
    if (file_header_create_helper(storfsInst, &firstPartInfo[1],
                                  storfsInst->cachedInfo.rootLocation[1],
                                  "Root") != STORFS_OK) {
      STORFS_LOGE(
          TAG, "The filesystem could not be created at location %ld%ld, %ld",
          (uint32_t)(storfsInst->cachedInfo.rootLocation[1].pageLoc >> 32),
          (uint32_t)(storfsInst->cachedInfo.rootLocation[1].pageLoc),
          storfsInst->cachedInfo.rootLocation[1].byteLoc);
      return STORFS_ERROR;
    }

    file_header_store_helper(storfsInst, &firstPartInfo[1],
                             storfsInst->cachedInfo.rootLocation[1], "Root");
    file_info_display_helper(firstPartInfo[1]);

    // Compare the CRC obtained from the file to the computed crc of the
    // filename
    if (crc_compare(storfsInst, firstPartInfo[1], firstPartInfo[1].fileName,
                    strLen) != STORFS_OK) {
      return STORFS_ERROR;
    }

    storfsInst->cachedInfo.rootHeaderInfo[0] = firstPartInfo[0];
    storfsInst->cachedInfo.rootHeaderInfo[1] = firstPartInfo[1];
  } else {
    // Get string length
    while (firstPartInfo[0].fileName[strLen++] != '\0')
      ;

    // Compare the CRC code to the register code
    if (crc_compare(storfsInst, firstPartInfo[0], firstPartInfo[0].fileName,
                    strLen) != STORFS_OK) {
      return STORFS_ERROR;
    }

    // Get string length
    strLen = 0;
    while (firstPartInfo[1].fileName[strLen++] != '\0')
      ;

    // Compare the CRC code to the register code
    if (crc_compare(storfsInst, firstPartInfo[1], firstPartInfo[1].fileName,
                    strLen) != STORFS_OK) {
      return STORFS_ERROR;
    }

    // Set next open byte
    storfsInst->cachedInfo.nextOpenByte = firstPartInfo[1].fragmentLocation;
  }

  return STORFS_OK;
}
