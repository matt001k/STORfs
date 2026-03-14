#include "core.h"

#include "common.h"
#include "crc.h"
#include "storfs.h"
#include "storfs_config.h"
#include "wear.h"

#include <string.h>

/** @brief Wear handling enum */
typedef enum {
  WRITE_GOOD = 0x0UL,
  WRITE_BAD,
  WRITE_RELOCATE,
} wear_level_state_t;

typedef enum {
  PATH_LAST = 0x0UL,
  PATH_LEFT,
} path_flag_t;

/** @brief Flags used for FILE struct */
#define STORFS_FILE_WRITE_FLAG        0x00000001
#define STORFS_FILE_READ_FLAG         0x00000002
#define STORFS_FILE_APPEND_FLAG       0x00000004
#define STORFS_FILE_PARENT_FLAG       0x00000008
#define STORFS_FILE_SIBLING_FLAG      0x00000010
#define STORFS_FILE_INIT_HEADER_WRITE 0x00000020
#define STORFS_FILE_HEADER_WRITE      0x00000040
#define STORFS_FILE_WRITE_INIT_FLAG   0x00000080
#define STORFS_FILE_REWIND_FLAG       0x00000100
#define STORFS_FILE_DELETED_FLAG      0xF1

static const char *TAG = "STORfs";

storfs_err_t update_root(storfs_t *storfsInst) {
  // Update both root registers
  if(storfsInst->erase(storfsInst,
                       storfsInst->cachedInfo.rootLocation[0].pageLoc) !=
     STORFS_OK) {
    return STORFS_ERROR;
  }
  file_header_create_helper(storfsInst,
                            &storfsInst->cachedInfo.rootHeaderInfo[0],
                            storfsInst->cachedInfo.rootLocation[0],
                            "Root Header 1");
  if(storfsInst->erase(storfsInst,
                       storfsInst->cachedInfo.rootLocation[1].pageLoc) !=
     STORFS_OK) {
    return STORFS_ERROR;
  }
  file_header_create_helper(storfsInst,
                            &storfsInst->cachedInfo.rootHeaderInfo[1],
                            storfsInst->cachedInfo.rootLocation[1],
                            "Root Header 2");

  return STORFS_OK;
}

storfs_err_t update_root_next_open_byte(storfs_t     *storfsInst,
                                        storfs_size_t fileLocation) {
  // Update the cached information with the next open byte
  storfsInst->cachedInfo.nextOpenByte = fileLocation;

  // Update the next open byte info in the cached rootInfo
  storfsInst->cachedInfo.rootHeaderInfo[0].fragmentLocation = fileLocation;
  storfsInst->cachedInfo.rootHeaderInfo[1].fragmentLocation = fileLocation;

  if(update_root(storfsInst) != STORFS_OK) {
    return STORFS_ERROR;
  }

  return STORFS_OK;
}

storfs_err_t find_update_next_open_byte(storfs_t    *storfsInst,
                                        storfs_loc_t storfsLoc) {
  STORFS_LOGD(TAG, "Finding and updating next open byte");
  if(find_next_open_byte_helper(storfsInst, &storfsLoc) != STORFS_OK) {
    return STORFS_ERROR;
  }

  // Update the next open byte available
  update_root_next_open_byte(storfsInst,
                             BYTEPAGE_TO_LOCATION(0,
                                                  storfsLoc.pageLoc,
                                                  storfsInst));

  return STORFS_OK;
}

void buf_to_info(uint8_t *buf, storfs_file_header_t *storfsInfo) {
  uint32_t i           = 0;
  storfsInfo->fileInfo = buf[i++];

  if((storfsInfo->fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == 0) {
    storfsInfo->reserved         = uint8_t_to_uint16_t(buf, &i);
    storfsInfo->fragmentLocation = uint8_t_to_uint64_t(buf, &i);
    storfsInfo->crc              = uint8_t_to_uint16_t(buf, &i);
    // Set all other information equal to zero
    for(int j = 0; j < STORFS_MAX_FILE_NAME; j++) {
      storfsInfo->fileName[j] = 0;
    }
    storfsInfo->childLocation   = 0;
    storfsInfo->siblingLocation = 0;
    storfsInfo->fileSize        = 0;
  } else {
    while(i < STORFS_MAX_FILE_NAME) {
      storfsInfo->fileName[i - STORFS_INFO_REG_SIZE] = buf[i];
      i++;
    }
    storfsInfo->childLocation    = uint8_t_to_uint64_t(buf, &i);
    storfsInfo->siblingLocation  = uint8_t_to_uint64_t(buf, &i);
    storfsInfo->reserved         = uint8_t_to_uint16_t(buf, &i);
    storfsInfo->fragmentLocation = uint8_t_to_uint64_t(buf, &i);
    storfsInfo->fileSize         = uint8_t_to_uint32_t(buf, &i);
    storfsInfo->crc              = uint8_t_to_uint16_t(buf, &i);
  }
}

void info_to_buf(uint8_t *buf, storfs_file_header_t *storfsInfo) {
  uint32_t i = 0;
  buf[i]     = storfsInfo->fileInfo;
  i++;

  if((storfsInfo->fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == 0) {
    uint16_t_to_uint8_t(buf, storfsInfo->reserved, &i);
    uint64_t_to_uint8_t(buf, storfsInfo->fragmentLocation, &i);
    uint16_t_to_uint8_t(buf, storfsInfo->crc, &i);
  } else {
    while(i < STORFS_MAX_FILE_NAME) {
      buf[i] = storfsInfo->fileName[i - STORFS_INFO_REG_SIZE];
      i++;
    }
    uint64_t_to_uint8_t(buf, storfsInfo->childLocation, &i);
    uint64_t_to_uint8_t(buf, storfsInfo->siblingLocation, &i);
    uint16_t_to_uint8_t(buf, storfsInfo->reserved, &i);
    uint64_t_to_uint8_t(buf, storfsInfo->fragmentLocation, &i);
    uint32_t_to_uint8_t(buf, storfsInfo->fileSize, &i);
    uint16_t_to_uint8_t(buf, storfsInfo->crc, &i);
  }
}

#ifdef STORFS_LOG_DISPLAY_HEADER
void file_info_display_helper(storfs_file_header_t storfsInfo) {
  STORFS_LOGI(TAG,
              "\t fileInfo %x \r\n \
        fileName %s \r\n \
        childLocation %lx%lx \r\n \
        siblingLocation %lx%lx \r\n \
        reserved %x \r\n \
        fragmentLocation/nextOpenByte %lx%lx \r\n \
        fileSize %lx \r\n \
        crc %x",
              storfsInfo.fileInfo,
              storfsInfo.fileName,
              (uint32_t)(storfsInfo.childLocation >> 32),
              (uint32_t)storfsInfo.childLocation,
              (uint32_t)(storfsInfo.siblingLocation >> 32),
              (uint32_t)storfsInfo.siblingLocation,
              storfsInfo.reserved,
              (uint32_t)(storfsInfo.fragmentLocation >> 32),
              (uint32_t)storfsInfo.fragmentLocation,
              storfsInfo.fileSize,
              storfsInfo.crc);
}
#else
void file_info_display_helper(storfs_file_header_t storfsInfo) {
  UNUSED(storfsInfo);
}
#endif

storfs_err_t file_header_create_helper(storfs_t             *storfsInst,
                                       storfs_file_header_t *storfsInfo,
                                       storfs_loc_t          storfsLoc,
                                       const char           *string) {
  storfs_err_t status = STORFS_OK;
  uint8_t      headerBuf[STORFS_HEADER_TOTAL_SIZE];

  // If header to create is overflowing the user defined page size or there is
  // no space left in the storage device return an error
  if(((storfsLoc.byteLoc + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize)) {
    status = STORFS_ERR_WRITE_FAILED;
    goto FUNEND;
  }

  // Turn the header information into a buffer to write to memory
  STORFS_LOGD(TAG,
              "Writing %s Header at %ld%ld, %ld",
              string,
              (uint32_t)(storfsLoc.pageLoc >> 32),
              (uint32_t)(storfsLoc.pageLoc),
              storfsLoc.byteLoc);
  info_to_buf(headerBuf, storfsInfo);
  if(storfsInst->write(storfsInst,
                       storfsLoc.pageLoc,
                       storfsLoc.byteLoc,
                       headerBuf,
                       STORFS_HEADER_TOTAL_SIZE) != STORFS_OK) {
    status = STORFS_ERR_WRITE_FAILED;
    goto FUNEND;
  }
  status = storfsInst->sync(storfsInst);

FUNEND:
  return status;
}

storfs_err_t file_header_store_helper(storfs_t             *storfsInst,
                                      storfs_file_header_t *storfsInfo,
                                      storfs_loc_t          storfsLoc,
                                      const char           *string) {
  storfs_err_t status = STORFS_OK;
  uint8_t      headerBuf[STORFS_HEADER_TOTAL_SIZE];

  STORFS_LOGD(TAG,
              "Storing %s Header at %ld%ld, %ld",
              string,
              (uint32_t)(storfsLoc.pageLoc >> 32),
              (uint32_t)(storfsLoc.pageLoc),
              storfsLoc.byteLoc);
  if(storfsInst->read(storfsInst,
                      storfsLoc.pageLoc,
                      storfsLoc.byteLoc,
                      headerBuf,
                      STORFS_HEADER_TOTAL_SIZE) != STORFS_OK) {
    status = STORFS_ERR_READ_FAILED;
    goto FUNEND;
  }
  status = storfsInst->sync(storfsInst);

  buf_to_info(headerBuf, storfsInfo);

FUNEND:
  return status;
}

storfs_err_t find_next_open_byte_helper(storfs_t     *storfsInst,
                                        storfs_loc_t *storfsLoc) {
  storfs_file_header_t nextHeaderInfo;
  nextHeaderInfo.fragmentLocation = 0;
  nextHeaderInfo.fileInfo         = 0x80;

  // Determine where the next open byte within the system is
  while(nextHeaderInfo.fragmentLocation != 0xFFFFFFFFFFFFFFFF ||
        nextHeaderInfo.siblingLocation != 0xFFFFFFFFFFFFFFFF ||
        nextHeaderInfo.childLocation != 0xFFFFFFFFFFFFFFFF ||
        nextHeaderInfo.fileInfo != 0xFF) {
    storfsLoc->pageLoc += 1;
    if(storfsLoc->byteLoc != 0) {
      storfsLoc->byteLoc = 0;
    }
    if(file_header_store_helper(storfsInst,
                                &nextHeaderInfo,
                                *storfsLoc,
                                "Next") != STORFS_OK) {
      return STORFS_ERROR;
    }
  }

  return STORFS_OK;
}

storfs_err_t file_handling_helper(storfs_t      *storfsInst,
                                  storfs_name_t *pathToDir,
                                  file_action_t  actionFlag,
                                  void          *buff) {
  int strLen = 0;  // String length of the path used
  int currStr;  // String length to hold the current file/directory in the path
  storfs_loc_t currentLocation =
      storfsInst->cachedInfo
          .rootLocation[0];  // Location of the current
                             // directory to obtain information
  STORFS_FILE previousFile;  // Information and location of the previous file
  uint8_t     fileSepCnt =
      0;  // File separator count to ensure files do no have children
  path_flag_t  pathFlag = PATH_LEFT;
  wear_level_t wearLevelInfo;
  uint8_t      updatedHeader[STORFS_HEADER_TOTAL_SIZE];

  while(1) {
    storfs_name_t currentFileName[STORFS_MAX_FILE_NAME];
    currStr = 0;
    while((pathToDir[strLen] != '/') && (pathToDir[strLen] != '\0')) {
      if(pathToDir[strLen] == '.') {
        if(actionFlag == DIR_CREATE) {
          STORFS_LOGE(TAG, "Directory name cannot have an extension");
          return STORFS_ERROR;
        }
        fileSepCnt++;
      }
      currentFileName[currStr++] = pathToDir[strLen++];
    }
    currentFileName[currStr] = '\0';
    STORFS_LOGD(TAG, "File name %s", currentFileName);

    if(pathToDir[strLen] == '\0') {
      pathFlag = PATH_LAST;
    }

    do {
      // Store the current file header
      if(file_header_store_helper(storfsInst,
                                  &wearLevelInfo.storfsInfo,
                                  currentLocation,
                                  "Directory") != STORFS_OK) {
        return STORFS_ERROR;
      }

      // Does the current file header name equal to the name in the path?
      if(strcmp((const char *)wearLevelInfo.storfsInfo.fileName,
                (const char *)currentFileName) == 0) {
        // If the filename is matched it is a parent directory, update the
        // previous file information with the current
        STORFS_LOGD(TAG, "File name matched: %s", currentFileName);

        if(pathFlag == PATH_LAST) {
          break;
        }

        // If there is no child location update the child's location to the next
        // open byte
        if(wearLevelInfo.storfsInfo.childLocation == 0x0) {
          wearLevelInfo.storfsInfo.childLocation =
              storfsInst->cachedInfo.nextOpenByte;
        }

        previousFile.fileLoc       = currentLocation;
        previousFile.filePrevLoc   = currentLocation;
        previousFile.fileInfo      = wearLevelInfo.storfsInfo;
        previousFile.filePrevFlags = STORFS_FILE_PARENT_FLAG;

        // Continue to search the child's location if it is not the last line
        // throughout the path
        currentLocation.pageLoc =
            LOCATION_TO_PAGE(wearLevelInfo.storfsInfo.childLocation,
                             storfsInst);
        currentLocation.byteLoc =
            LOCATION_TO_BYTE(wearLevelInfo.storfsInfo.childLocation,
                             storfsInst);
      } else if(wearLevelInfo.storfsInfo.siblingLocation !=
                0xFFFFFFFFFFFFFFFF) {
        // If there is no sibling location update the sibling's location to the
        // next open byte
        if(wearLevelInfo.storfsInfo.siblingLocation == 0x0) {
          wearLevelInfo.storfsInfo.siblingLocation =
              storfsInst->cachedInfo.nextOpenByte;
        }

        // If the filename is not matched and the sibling location exists, it is
        // a sibling directory Update the previous file information with the
        // current
        STORFS_LOGD(TAG, "Name not matched, searching siblings");
        previousFile.fileLoc       = currentLocation;
        previousFile.filePrevLoc   = currentLocation;
        previousFile.filePrevFlags = STORFS_FILE_SIBLING_FLAG;
        previousFile.fileInfo      = wearLevelInfo.storfsInfo;

        // Continue to search the siblings's location
        currentLocation.pageLoc =
            LOCATION_TO_PAGE(wearLevelInfo.storfsInfo.siblingLocation,
                             storfsInst);
        currentLocation.byteLoc =
            LOCATION_TO_BYTE(wearLevelInfo.storfsInfo.siblingLocation,
                             storfsInst);
      } else {
        STORFS_LOGD(TAG,
                    "Name not matched, and no siblings, creating "
                    "file/directory at next open location");

        // Error is next write is larger than the page count
        if(LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) >=
           storfsInst->pageCount) {
          STORFS_LOGE(TAG, "Cannot write any more data to the file system");
        }

        // Files cannot be children of other files
        if(fileSepCnt > 1) {
          STORFS_LOGE(TAG, "File/directory cannot be a child of another file");
          return STORFS_ERROR;
        }

        // Set information for the header of the current file directory
        for(int i = 0; i <= currStr; i++) {
          wearLevelInfo.storfsInfo.fileName[i] = currentFileName[i];
        }
        wearLevelInfo.storfsInfo.reserved = 0xFFFF;
        wearLevelInfo.storfsInfo.fileSize = STORFS_HEADER_TOTAL_SIZE;

        // Set the current directory fragment, sibling and child location
        // registers to zero These locations will never be zero as long as the
        // file system exists, it is a safe value to set these
        wearLevelInfo.storfsInfo.siblingLocation  = 0;
        wearLevelInfo.storfsInfo.childLocation    = 0;
        wearLevelInfo.storfsInfo.fragmentLocation = 0;

        // Compute CRC of the filename given
        wearLevelInfo.storfsInfo.crc =
            STORFS_CRC_CALC(storfsInst,
                            wearLevelInfo.storfsInfo.fileName,
                            (currStr + 1));

        // If the file size will be the size of the page size, ensure the file
        // info sets the information for the file to full
        if(actionFlag == DIR_CREATE) {
          wearLevelInfo.storfsInfo.fileInfo =
              STORFS_INFO_REG_FILE_TYPE_DIRECTORY |
              STORFS_INFO_REG_BLOCK_SIGN_FULL;
        } else {
          wearLevelInfo.storfsInfo.fileInfo =
              STORFS_INFO_REG_FILE_TYPE_FILE |
              STORFS_INFO_REG_BLOCK_SIGN_PART_FULL;
        }

        info_to_buf(updatedHeader, &wearLevelInfo.storfsInfo);
        wearLevelInfo.sendBuf       = updatedHeader;
        wearLevelInfo.headerLen     = STORFS_HEADER_TOTAL_SIZE;
        wearLevelInfo.sendDataLen   = STORFS_HEADER_TOTAL_SIZE;
        wearLevelInfo.storfsCurrLoc = &currentLocation;
        wearLevelInfo.storfsInfoLoc = currentLocation;
        wearLevelInfo.storfsOrigLoc = currentLocation;
        wearLevelInfo.storfsPrevLoc = previousFile.fileLoc;
        wearLevelInfo.storfsFlags =
            STORFS_FILE_INIT_HEADER_WRITE | previousFile.filePrevFlags;

        // Write the new header to the needed location in flash
        write_wear_level_helper(storfsInst, &wearLevelInfo);

        // Display the newly created file information
        file_info_display_helper(wearLevelInfo.storfsInfo);

        // Determine the next open byte for the cache and update root header if
        // needed
        if(find_update_next_open_byte(storfsInst, currentLocation) !=
           STORFS_OK) {
          return STORFS_ERROR;
        }
        break;
      }
    } while(previousFile.filePrevFlags == STORFS_FILE_SIBLING_FLAG);

    if(pathFlag == PATH_LAST) {
      break;
    }

    strLen += 1;
  }

  // If action is to open the file and store the memory location to the buffer
  // passed in
  if(actionFlag == FILE_OPEN) {
    // If the file was just created and file action is FILE_OPEN ensure that the
    // current location is used as the previous location
    previousFile.fileLoc  = currentLocation;
    previousFile.fileInfo = wearLevelInfo.storfsInfo;
    *(STORFS_FILE *)buff  = previousFile;
  }

  return STORFS_OK;
}

storfs_err_t file_delete_helper(storfs_t            *storfsInst,
                                storfs_loc_t         storfsLoc,
                                storfs_file_header_t storfsInfo) {
  int32_t      delDataItr = 0;
  storfs_loc_t delDataHeaderLoc =
      storfsLoc;  // Location of the file to be removed
  storfs_file_header_t currHeaderInfo =
      storfsInfo;  // Information of the file to be removed

  // Determine the number of iterations for deletion of files
  delDataItr =
      (storfsInfo.fileSize + storfsInst->pageSize) / storfsInst->pageSize;
  delDataHeaderLoc.byteLoc = 0;

  do {
    STORFS_LOGD(TAG,
                "Deleting File/Fragment At %ld%ld, %ld",
                (uint32_t)(delDataHeaderLoc.pageLoc >> 32),
                (uint32_t)(delDataHeaderLoc.pageLoc),
                delDataHeaderLoc.byteLoc);

    // Erase the current page
    if(storfsInst->erase(storfsInst, delDataHeaderLoc.pageLoc) != STORFS_OK) {
      STORFS_LOGE(TAG, "Erasing page failed in function remove");
      return STORFS_ERROR;
    }

    delDataItr--;
    if(delDataItr > 0) {
      // Set the next location to what is in the erased page's header
      delDataHeaderLoc.pageLoc =
          LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);

      // Store the next locations header information
      if(file_header_store_helper(storfsInst,
                                  &currHeaderInfo,
                                  delDataHeaderLoc,
                                  "") != STORFS_OK) {
        STORFS_LOGE(TAG, "Could not read from the current header");
        return STORFS_ERROR;
      }
    }
  } while(delDataItr > 0);

  return STORFS_OK;
}

storfs_err_t storfs_display_header(storfs_t *storfsInst, storfs_loc_t loc) {
  storfs_file_header_t header;
  if(file_header_store_helper(storfsInst, &header, loc, "Test") != STORFS_OK) {
    return STORFS_ERROR;
  }
  file_info_display_helper(header);

  return STORFS_OK;
}
