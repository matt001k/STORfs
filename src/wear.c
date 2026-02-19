#include "wear.h"

#include "core.h"
#include "crc.h"

#include <stdint.h>

/** @brief Wear handling enum */
typedef enum {
  WRITE_GOOD = 0x0UL,
  WRITE_BAD,
  WRITE_RELOCATE,
} wear_level_state_t;

storfs_err_t find_prev_file_loc(storfs_t     *storfsInst,
                                storfs_loc_t  storfsCurrLoc,
                                storfs_loc_t  storfsItrLoc,
                                storfs_loc_t *storfsPrevLoc) {
  storfs_file_header_t prevFileHeader;
  storfs_loc_t         storfsNextChildLoc;
  storfs_loc_t         storfsNextSibLoc;

  // Store the iterator header and determine if the child or sibling location is
  // equal to the the current location
  if(file_header_store_helper(storfsInst,
                              &prevFileHeader,
                              storfsItrLoc,
                              "Previous File") != STORFS_OK) {
    return STORFS_ERROR;
  }
  if(prevFileHeader.childLocation == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc,
                                                          storfsCurrLoc.pageLoc,
                                                          storfsInst) ||
     prevFileHeader.siblingLocation ==
         BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc,
                              storfsCurrLoc.pageLoc,
                              storfsInst)) {
    *storfsPrevLoc = storfsItrLoc;
    return STORFS_OK;
  }

  // Determine whether the previous file has a child
  if(prevFileHeader.childLocation != 0x00) {
    storfsNextChildLoc.byteLoc =
        LOCATION_TO_BYTE(prevFileHeader.childLocation, storfsInst);
    storfsNextChildLoc.pageLoc =
        LOCATION_TO_PAGE(prevFileHeader.childLocation, storfsInst);

    // If the previous file has a child re-iterate through find_prev_file_loc
    if(find_prev_file_loc(storfsInst,
                          storfsCurrLoc,
                          storfsNextChildLoc,
                          storfsPrevLoc) != STORFS_OK) {
      return STORFS_ERROR;
    }

    // If the previously found location is the same as the
    if(BYTEPAGE_TO_LOCATION(storfsPrevLoc->byteLoc,
                            storfsPrevLoc->pageLoc,
                            storfsInst) ==
       BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc,
                            storfsCurrLoc.pageLoc,
                            storfsInst)) {
      return STORFS_OK;
    }
  }

  // Determine whether the previous file has a sibling
  if(prevFileHeader.siblingLocation != 0x00) {
    // If the current header has a sibling location, continuously iterate
    // through the siblings until the wanted location is found or not
    while(prevFileHeader.siblingLocation != 0x00) {
      storfsNextSibLoc.byteLoc =
          LOCATION_TO_BYTE(prevFileHeader.siblingLocation, storfsInst);
      storfsNextSibLoc.pageLoc =
          LOCATION_TO_PAGE(prevFileHeader.siblingLocation, storfsInst);
      if(file_header_store_helper(storfsInst,
                                  &prevFileHeader,
                                  storfsNextSibLoc,
                                  "Previous File") != STORFS_OK) {
        return STORFS_ERROR;
      }

      // If either the child location of the sibling location is equivalent to
      // the wanted location, place that
      if(prevFileHeader.childLocation ==
             BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc,
                                  storfsCurrLoc.pageLoc,
                                  storfsInst) ||
         prevFileHeader.siblingLocation ==
             BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc,
                                  storfsCurrLoc.pageLoc,
                                  storfsInst)) {
        *storfsPrevLoc = storfsNextSibLoc;
        return STORFS_OK;
      }

      // If there is a child location, iterate through it with the prev_file_loc
      // function
      if(prevFileHeader.childLocation != 0x00) {
        storfsNextChildLoc.byteLoc =
            LOCATION_TO_BYTE(prevFileHeader.childLocation, storfsInst);
        storfsNextChildLoc.pageLoc =
            LOCATION_TO_PAGE(prevFileHeader.childLocation, storfsInst);
        if(find_prev_file_loc(storfsInst,
                              storfsCurrLoc,
                              storfsNextChildLoc,
                              storfsPrevLoc) != STORFS_OK) {
          return STORFS_ERROR;
        }
        if(BYTEPAGE_TO_LOCATION(storfsPrevLoc->byteLoc,
                                storfsPrevLoc->pageLoc,
                                storfsInst) ==
           BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc,
                                storfsCurrLoc.pageLoc,
                                storfsInst)) {
          return STORFS_OK;
        }
      }
    }
  }

  return STORFS_OK;
}

storfs_err_t wear_level_act(storfs_t *storfsInst, wear_level_t *wearLevelInfo) {
  uint8_t      relocateBuf[storfsInst->pageSize];
  wear_level_t prevWearLevelInfo;

  // Store the previous header, determine if it was a fragment header or a
  // file/directory/root header
  file_header_store_helper(storfsInst,
                           &prevWearLevelInfo.storfsInfo,
                           wearLevelInfo->storfsPrevLoc,
                           "Previous File");

  // If the previous file information is a file, the root or a directory... else
  // if it is a fragment
  if((prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
         STORFS_INFO_REG_FILE_TYPE_FILE ||
     (prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
         STORFS_INFO_REG_FILE_TYPE_ROOT ||
     (prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
         STORFS_INFO_REG_FILE_TYPE_DIRECTORY) {
    // Determine the parent/sibling location of the previous file in order to
    // prepare it for another iteration of wear-level writing
    if(find_prev_file_loc(storfsInst,
                          wearLevelInfo->storfsPrevLoc,
                          storfsInst->cachedInfo.rootLocation[0],
                          &prevWearLevelInfo.storfsPrevLoc) != STORFS_OK) {
      STORFS_LOGE(
          TAG,
          "Error determining the previous file's parent/sibling location");
      return STORFS_ERROR;
    }

    // The header info location will be the same as the previous location
    prevWearLevelInfo.storfsInfoLoc = prevWearLevelInfo.storfsPrevLoc;

    // Set the previous header length
    prevWearLevelInfo.headerLen = STORFS_HEADER_TOTAL_SIZE;

    // Set the page size that must be re-written
    if(prevWearLevelInfo.storfsInfo.fileSize > storfsInst->pageSize) {
      prevWearLevelInfo.sendDataLen = storfsInst->pageSize;
    } else {
      prevWearLevelInfo.sendDataLen = prevWearLevelInfo.storfsInfo.fileSize;
    }

    // Determine if the child location or sibling location must be re-written
    if(prevWearLevelInfo.storfsInfo.childLocation ==
           BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsOrigLoc.byteLoc,
                                wearLevelInfo->storfsOrigLoc.pageLoc,
                                storfsInst) ||
       wearLevelInfo->storfsFlags & STORFS_FILE_PARENT_FLAG) {
      STORFS_LOGI(TAG, "Updating previous file child location");
      prevWearLevelInfo.storfsInfo.childLocation =
          BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc,
                               wearLevelInfo->storfsCurrLoc->pageLoc,
                               storfsInst);
    } else if(prevWearLevelInfo.storfsInfo.siblingLocation ==
                  BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsOrigLoc.byteLoc,
                                       wearLevelInfo->storfsOrigLoc.pageLoc,
                                       storfsInst) ||
              wearLevelInfo->storfsFlags & STORFS_FILE_SIBLING_FLAG) {
      STORFS_LOGI(TAG, "Updating previous file sibling location");
      prevWearLevelInfo.storfsInfo.siblingLocation =
          BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc,
                               wearLevelInfo->storfsCurrLoc->pageLoc,
                               storfsInst);
    }
  } else {
    storfs_file_header_t tempHeader = wearLevelInfo->storfsInfo;

    // If the current head file header's fragment location is equal to the
    // previous files fragment location
    if(tempHeader.fragmentLocation ==
       BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsPrevLoc.byteLoc,
                            wearLevelInfo->storfsPrevLoc.pageLoc,
                            storfsInst)) {
      // The previous location will be equivalent to the main header's location
      prevWearLevelInfo.storfsPrevLoc = wearLevelInfo->storfsInfoLoc;
    } else {
      // Iterate through fragment header locations until the previous fragment
      // header is found
      while(tempHeader.fragmentLocation !=
            BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsPrevLoc.byteLoc,
                                 wearLevelInfo->storfsPrevLoc.pageLoc,
                                 storfsInst)) {
        prevWearLevelInfo.storfsPrevLoc.pageLoc =
            LOCATION_TO_PAGE(tempHeader.fragmentLocation, storfsInst);
        prevWearLevelInfo.storfsPrevLoc.byteLoc =
            LOCATION_TO_BYTE(tempHeader.fragmentLocation, storfsInst);

        // Update the prevWearLevel header information while iterating
        file_header_store_helper(storfsInst,
                                 &tempHeader,
                                 prevWearLevelInfo.storfsPrevLoc,
                                 "Previous Fragment");
      }
    }

    STORFS_LOGI(TAG, "Updating previous file fragment location");

    // Set the previous header length to be a fragment header length
    prevWearLevelInfo.headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;

    // Set the previous header's info location to the original
    prevWearLevelInfo.storfsInfoLoc = wearLevelInfo->storfsInfoLoc;

    // Set the page size that must be re-written, must be a full page if there
    // is a fragment
    prevWearLevelInfo.sendDataLen = storfsInst->pageSize;

    // Update the fragment location
    prevWearLevelInfo.storfsInfo.fragmentLocation =
        BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc,
                             wearLevelInfo->storfsCurrLoc->pageLoc,
                             storfsInst);
  }

  // Determine if the previous file is a directory or the root and ensure that
  // the correct flag is applied to the next iteration
  if((prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
         STORFS_INFO_REG_FILE_TYPE_DIRECTORY ||
     (prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
         STORFS_INFO_REG_FILE_TYPE_ROOT ||
     (((prevWearLevelInfo.storfsInfo.fileInfo &
        STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_FILE) &&
      (prevWearLevelInfo.sendDataLen == STORFS_HEADER_TOTAL_SIZE))) {
    prevWearLevelInfo.storfsFlags = STORFS_FILE_HEADER_WRITE;
  } else {
    prevWearLevelInfo.storfsFlags = STORFS_FILE_WRITE_FLAG;
  }

  STORFS_LOGI(
      TAG,
      "Previous file's, previous file location %ld%ld",
      (uint32_t)(BYTEPAGE_TO_LOCATION(prevWearLevelInfo.storfsPrevLoc.byteLoc,
                                      prevWearLevelInfo.storfsPrevLoc.pageLoc,
                                      storfsInst) >>
                 32),
      (uint32_t)(BYTEPAGE_TO_LOCATION(prevWearLevelInfo.storfsPrevLoc.byteLoc,
                                      prevWearLevelInfo.storfsPrevLoc.pageLoc,
                                      storfsInst)));

  // Convert the header to a buffer, read the previous file, erase it and write
  // the new information to it
  file_info_display_helper(prevWearLevelInfo.storfsInfo);
  info_to_buf(relocateBuf, &prevWearLevelInfo.storfsInfo);
  if(storfsInst->read(storfsInst,
                      wearLevelInfo->storfsPrevLoc.pageLoc,
                      prevWearLevelInfo.headerLen,
                      (relocateBuf + prevWearLevelInfo.headerLen),
                      (storfsInst->pageSize - prevWearLevelInfo.headerLen)) !=
     STORFS_OK) {
    return STORFS_ERR_READ_FAILED;
  }
  if(storfsInst->erase(storfsInst, wearLevelInfo->storfsPrevLoc.pageLoc) !=
     STORFS_OK) {
    return STORFS_ERROR;
  }

  // Write to the previous wear struct the previous files information
  prevWearLevelInfo.sendBuf       = relocateBuf;
  prevWearLevelInfo.storfsCurrLoc = &wearLevelInfo->storfsPrevLoc;
  prevWearLevelInfo.storfsOrigLoc = wearLevelInfo->storfsPrevLoc;

  // Write the previous file and header
  write_wear_level_helper(storfsInst, &prevWearLevelInfo);

  return STORFS_OK;
}

storfs_err_t write_wear_level_helper(storfs_t     *storfsInst,
                                     wear_level_t *wearLevelInfo) {
  wear_level_state_t state = WRITE_BAD;
  uint8_t            itr   = 0;

  // Write to the area in memory and then check the crc and determine if that
  // page in memory is worn/not usable
  while(1) {
    STORFS_LOGD(TAG,
                "Writing File At %ld%ld, %ld",
                (uint32_t)(wearLevelInfo->storfsCurrLoc->pageLoc >> 32),
                (uint32_t)(wearLevelInfo->storfsCurrLoc->pageLoc),
                wearLevelInfo->storfsCurrLoc->byteLoc);

    // Retry write if failed to the page a certain amount of times based on user
    // defined value
    for(int i = 0; i < STORFS_WEAR_LEVEL_RETRY_NUM; i++) {
      if(i > 0) {
        STORFS_LOGW(TAG,
                    "Failed to write to location, re-writting to location");
      }

      // If the programming functionality fails return an error
      if(storfsInst->write(storfsInst,
                           wearLevelInfo->storfsCurrLoc->pageLoc,
                           wearLevelInfo->storfsCurrLoc->byteLoc,
                           wearLevelInfo->sendBuf,
                           wearLevelInfo->sendDataLen) != STORFS_OK) {
        STORFS_LOGE(TAG, "Writing to memory failed in function fputs");
        return STORFS_ERR_WRITE_FAILED;
      }
      if(storfsInst->sync(storfsInst) != STORFS_OK) {
        return STORFS_ERROR;
      }
      if(wearLevelInfo->storfsFlags & STORFS_FILE_INIT_HEADER_WRITE ||
         wearLevelInfo->storfsFlags & STORFS_FILE_HEADER_WRITE) {
        // If CRC returned correctly, break
        if(crc_header_check(storfsInst, *wearLevelInfo->storfsCurrLoc) ==
           STORFS_OK) {
          if(itr == 0) {
            state = WRITE_GOOD;
          } else {
            state = WRITE_RELOCATE;
          }
          break;
        }
      } else {
        if(crc_file_check(storfsInst,
                          *wearLevelInfo->storfsCurrLoc,
                          (wearLevelInfo->sendDataLen -
                           wearLevelInfo->headerLen)) == STORFS_OK) {
          if(itr == 0) {
            state = WRITE_GOOD;
          } else {
            state = WRITE_RELOCATE;
          }
          break;
        }
      }
      if(storfsInst->erase(storfsInst, wearLevelInfo->storfsCurrLoc->pageLoc) !=
         STORFS_OK) {
        STORFS_LOGE(TAG, "Could not erase page in wear-level function");
        return STORFS_ERROR;
      }
    }

    // If written successful, continue
    if(state == WRITE_GOOD || state == WRITE_RELOCATE) {
      break;
    }

    // If CRC returns incorrectly, find another location to write to
    find_next_open_byte_helper(storfsInst, wearLevelInfo->storfsCurrLoc);

    // If this is a file being written to, it is the first write and the send
    // data length is greater than a page size, the fragment location must be
    // updated as well
    if(wearLevelInfo->storfsFlags & STORFS_FILE_WRITE_FLAG &&
       wearLevelInfo->storfsFlags & STORFS_FILE_WRITE_INIT_FLAG &&
       wearLevelInfo->sendDataLen >= storfsInst->pageSize) {
      storfs_loc_t         nextFragmentLoc = *wearLevelInfo->storfsCurrLoc;
      storfs_file_header_t currInfo;

      find_next_open_byte_helper(storfsInst, &nextFragmentLoc);
      buf_to_info(wearLevelInfo->sendBuf, &currInfo);
      currInfo.fragmentLocation = BYTEPAGE_TO_LOCATION(nextFragmentLoc.byteLoc,
                                                       nextFragmentLoc.pageLoc,
                                                       storfsInst);
      info_to_buf(wearLevelInfo->sendBuf, &currInfo);
    }

    itr++;
  }

  // If a file was rewritten to a new location than what was expected, the
  // previous file must be re-written with new location Or if it is the initial
  // write to a header file, the previous file must be updated to the newest
  // position
  if(state == WRITE_RELOCATE ||
     wearLevelInfo->storfsFlags & STORFS_FILE_INIT_HEADER_WRITE) {
    if(wearLevelInfo->storfsPrevLoc.pageLoc ==
           storfsInst->cachedInfo.rootLocation[0].pageLoc &&
       wearLevelInfo->storfsPrevLoc.byteLoc ==
           storfsInst->cachedInfo.rootLocation[0].byteLoc) {
      storfsInst->cachedInfo.rootHeaderInfo[0].childLocation =
          BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc,
                               wearLevelInfo->storfsCurrLoc->pageLoc,
                               storfsInst);
      storfsInst->cachedInfo.rootHeaderInfo[1].childLocation =
          BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc,
                               wearLevelInfo->storfsCurrLoc->pageLoc,
                               storfsInst);
      return STORFS_OK;
    }
    wear_level_act(storfsInst, wearLevelInfo);
  }

  return STORFS_OK;
}
