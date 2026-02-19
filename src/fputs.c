#include "core.h"
#include "crc.h"
#include "storfs.h"
#include "wear.h"

storfs_err_t storfs_fputs(storfs_t *storfsInst, const char *str,
                          const storfs_size_t n, STORFS_FILE *stream) {
  // Sanity Check
  if (!storfsInst || !stream || !str || !stream) {
    return STORFS_ERR_NULL_POINTER;
  }

  if (n == 0) {
    return STORFS_ERR_INVALID_PARAM;
  }

  // Error if next write is larger than the page count
  if (LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) >=
      storfsInst->pageCount) {
    STORFS_LOGE(TAG, "Cannot write any more data to the file system");
  }

  // If the file is opened in read only return an error
  if (stream->fileFlags == STORFS_FILE_READ_FLAG) {
    STORFS_LOGE(TAG, "Cannot write to file, in read only mode");
    return STORFS_ERROR;
  }

  STORFS_LOGI(TAG, "Writing to file %s", stream->fileInfo.fileName);

  wear_level_t wearLevelInfo; // Needed for wear level writing
  uint8_t
      sendBuf[storfsInst->pageSize]; // Buffer of data to send to flash device
  uint8_t headerBuf[STORFS_HEADER_TOTAL_SIZE]; // Buffer used to store the
                                               // header of each page
  uint32_t headerLen =
      STORFS_HEADER_TOTAL_SIZE; // Length of header to be used depending on
                                // fragment header or file header
  storfs_size_t count = n;      // Length of the data to be placed in storage
  int32_t sendDataItr = 0;      // Iterations for the number of pages to be
                                // programmed
  int32_t currItr = 0;

  storfs_loc_t currDataHeaderLoc =
      stream->fileLoc; // Location of the current data
  storfs_loc_t nextDataHeaderLoc =
      currDataHeaderLoc; // Next location for data to be written to
  storfs_loc_t prevDataHeaderLoc = stream->filePrevLoc;

  storfs_file_size_t
      updatedFileSize; // Updated filesize to be written to the header
  storfs_file_header_t currHeaderInfo; // Current Header's information

  int32_t appendHeaderByteLoc =
      0; // Location of the data to be appended onto the current buffer

  // Get updated file information
  if (file_header_store_helper(storfsInst, &stream->fileInfo, stream->fileLoc,
                               "Updated") != STORFS_OK) {
    return STORFS_ERROR;
  }

  if (stream->fileFlags & STORFS_FILE_APPEND_FLAG &&
      stream->fileInfo.fileSize > STORFS_HEADER_TOTAL_SIZE &&
      !(stream->fileFlags & STORFS_FILE_REWIND_FLAG)) {
    // Update the file size of the main header
    updatedFileSize =
        stream->fileInfo.fileSize + n +
        ((n / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) *
         STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

    // Store the file header
    currHeaderInfo = stream->fileInfo;

    // Find the current location of the header to be appended on to
    currDataHeaderLoc.byteLoc = 0;
    while (currHeaderInfo.fragmentLocation != 0x00) {
      // Set the previous header location to the current
      prevDataHeaderLoc = currDataHeaderLoc;

      // Set the current header location to the fragment location
      currDataHeaderLoc.pageLoc =
          LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);
      file_header_store_helper(storfsInst, &currHeaderInfo, currDataHeaderLoc,
                               "Append");
    }

    // If the location of the next available byte is greater than the files
    // original location...
    if (currDataHeaderLoc.pageLoc != stream->fileLoc.pageLoc) {
      STORFS_LOGD(TAG, "Appending to file fragment");

      // Append needed to write onto the current buffer length
      appendHeaderByteLoc = (stream->fileInfo.fileSize % storfsInst->pageSize) -
                            STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
      if (appendHeaderByteLoc < 0) {
        appendHeaderByteLoc = 0;
      }

      // Update the file size register in the header of the file
      stream->fileInfo.fileSize = updatedFileSize;
      if (storfsInst->read(
              storfsInst, stream->fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE,
              (sendBuf + STORFS_HEADER_TOTAL_SIZE),
              (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK) {
        return STORFS_ERR_READ_FAILED;
      }

      // Delete the file header so it may be written to
      if (storfsInst->erase(storfsInst, stream->fileLoc.pageLoc) != STORFS_OK) {
        return STORFS_ERROR;
      }

      // Write the new file header with the updated information
      info_to_buf(sendBuf, &stream->fileInfo);
      if (storfsInst->write(storfsInst, stream->fileLoc.pageLoc, 0, sendBuf,
                            storfsInst->pageSize) != STORFS_OK) {
        return STORFS_ERR_WRITE_FAILED;
      }

      // Read in the current header of the data buffer
      if (storfsInst->read(storfsInst, currDataHeaderLoc.pageLoc,
                           STORFS_FRAGMENT_HEADER_TOTAL_SIZE,
                           (sendBuf + STORFS_FRAGMENT_HEADER_TOTAL_SIZE),
                           (appendHeaderByteLoc -
                            STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) != STORFS_OK) {
        return STORFS_ERR_READ_FAILED;
      }
      // Delete the page from memory so it may be re-written
      if (storfsInst->erase(storfsInst, currDataHeaderLoc.pageLoc) !=
          STORFS_OK) {
        return STORFS_ERROR;
      }

      // Set the header length to fragment header size
      headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
    } else {
      STORFS_LOGD(TAG, "Appending to file head");

      // Append needed to write onto the current buffer length
      appendHeaderByteLoc =
          stream->fileInfo.fileSize - STORFS_HEADER_TOTAL_SIZE;
      if (appendHeaderByteLoc < 0) {
        appendHeaderByteLoc = 0;
      }

      // Read in the current header of the data buffer
      if (storfsInst->read(
              storfsInst, stream->fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE,
              (sendBuf + STORFS_HEADER_TOTAL_SIZE),
              (appendHeaderByteLoc - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK) {
        return STORFS_ERR_READ_FAILED;
      }
      // Delete the page from memory so it may be re-written
      if (storfsInst->erase(storfsInst, stream->fileLoc.pageLoc) != STORFS_OK) {
        return STORFS_ERROR;
      }

      // Set the current filesize information
      currHeaderInfo.fileSize = updatedFileSize;
    }

    // Adjust the count of data to be written to
    count += appendHeaderByteLoc;

    STORFS_LOGD(TAG, "Append File Location: %ld%ld, %ld",
                (uint32_t)(currDataHeaderLoc.pageLoc >> 32),
                (uint32_t)currDataHeaderLoc.pageLoc,
                appendHeaderByteLoc + headerLen);

    // Determine the number of iterations that must be programmed to the device
    sendDataItr = (count + storfsInst->pageSize) /
                  (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

    // Set the next data header location to this location
    nextDataHeaderLoc = currDataHeaderLoc;

    // Adjust reading file size remainder
    stream->fileRead.fileSizeRem -= appendHeaderByteLoc;
  } else {
    // Store the current header so it may be updated when initially writting to
    // memory
    currHeaderInfo = stream->fileInfo;

    // Delete the file to be written to
    file_delete_helper(storfsInst, currDataHeaderLoc, currHeaderInfo);

    // Update the file size register
    updatedFileSize =
        STORFS_HEADER_TOTAL_SIZE + n +
        ((n / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) *
         STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

    // Update the file size register in the header of the file
    currHeaderInfo.fileSize = updatedFileSize;

    // Determine the number of iterations that must be programmed to the device
    sendDataItr = 1;
    if ((count + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize) {
      sendDataItr +=
          ((count - (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) +
           storfsInst->pageSize) /
          (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE);
    }

    // Reset reading file size remainder and file read pointer
    stream->fileRead.fileSizeRem = 0;
    stream->fileRead.readLocPtr.pageLoc = stream->fileLoc.pageLoc;
    stream->fileRead.readLocPtr.byteLoc = STORFS_HEADER_TOTAL_SIZE;
  }

  do {
    // Determine which type of header to store
    if (currItr > 0) {
      headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
      currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_FILE_TYPE_FILE);
      currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_NOT_FRAGMENT_BIT);
    } else {
      currHeaderInfo.fileInfo |= STORFS_INFO_REG_NOT_FRAGMENT_BIT;
    }

    // If the string length is greater than a page size ensure the sent data can
    // maximally be the page size
    if ((count + headerLen) > storfsInst->pageSize) {
      wearLevelInfo.sendDataLen = storfsInst->pageSize;
      count -= (storfsInst->pageSize - headerLen);

      // Determine where the fragment location will be at
      if ((storfsInst->cachedInfo.nextOpenByte <
           BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc,
                                currDataHeaderLoc.pageLoc, storfsInst)) &&
          currItr == 0) {
        nextDataHeaderLoc.pageLoc =
            LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst);
      } else {
        find_next_open_byte_helper(storfsInst, &nextDataHeaderLoc);
      }

      // Set the file header to full in the current header
      currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_BLOCK_SIGN_EMPTY);
      currHeaderInfo.fileInfo |= STORFS_INFO_REG_BLOCK_SIGN_FULL;

      // Update the fragment register in the previous header with the new
      // fragment location
      currHeaderInfo.fragmentLocation = BYTEPAGE_TO_LOCATION(
          nextDataHeaderLoc.byteLoc, nextDataHeaderLoc.pageLoc, storfsInst);

    } else {
      wearLevelInfo.sendDataLen = count + headerLen;

      // If the total size is written to the page then set the file info flag as
      // block full of data
      if (wearLevelInfo.sendDataLen == storfsInst->pageSize) {
        currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_BLOCK_SIGN_EMPTY);
        currHeaderInfo.fileInfo |= STORFS_INFO_REG_BLOCK_SIGN_FULL;
      } else {
        currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_BLOCK_SIGN_EMPTY);
        currHeaderInfo.fileInfo |= STORFS_INFO_REG_BLOCK_SIGN_PART_FULL;
      }

      currHeaderInfo.fragmentLocation = 0x00;
    }

    // Convert the current header info into a buffer and store it in the first
    // bytes to be programmed Store the data to be programmed as well in the
    // buffer
    for (storfs_size_t i = headerLen; i < wearLevelInfo.sendDataLen; i++) {
      // If there is items to append to the current buffer
      if (currItr == 0 &&
          ((i - headerLen) < (storfs_size_t)appendHeaderByteLoc)) {
        continue;
      } else {
        sendBuf[i] = str[i - headerLen - appendHeaderByteLoc];
      }
    }

    // Calculate CRC
    currHeaderInfo.crc =
        STORFS_CRC_CALC(storfsInst, (uint8_t *)(sendBuf + headerLen),
                        (wearLevelInfo.sendDataLen - headerLen));

    // Place Header into buffer
    info_to_buf(headerBuf, &currHeaderInfo);
    for (storfs_size_t i = 0; i < headerLen; i++) {
      sendBuf[i] = headerBuf[i];
    }

    // Wear level handling for information
    wearLevelInfo.headerLen = headerLen;
    wearLevelInfo.sendBuf = sendBuf;
    wearLevelInfo.storfsCurrLoc = &currDataHeaderLoc;
    wearLevelInfo.storfsOrigLoc = currDataHeaderLoc;
    wearLevelInfo.storfsPrevLoc = prevDataHeaderLoc;
    wearLevelInfo.storfsInfo = stream->fileInfo;
    wearLevelInfo.storfsInfoLoc = stream->fileLoc;
    wearLevelInfo.storfsFlags =
        STORFS_FILE_WRITE_FLAG | STORFS_FILE_WRITE_INIT_FLAG;
    if (write_wear_level_helper(storfsInst, &wearLevelInfo) != STORFS_OK) {
      return STORFS_ERROR;
    }

    // Decrement the number of iterations left
    --sendDataItr;

    // Increment the buffer's location to send data
    str += (wearLevelInfo.sendDataLen - headerLen - appendHeaderByteLoc) *
           sizeof(uint8_t);

    // Set current header location equal to the next, and previous to current
    if (wearLevelInfo.storfsCurrLoc->pageLoc >= nextDataHeaderLoc.pageLoc) {
      // Update nextOpenByte to what is available
      find_next_open_byte_helper(storfsInst, &currDataHeaderLoc);
      storfsInst->cachedInfo.nextOpenByte = BYTEPAGE_TO_LOCATION(
          currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc, storfsInst);

      // If the current header location is equivalent to the stream's original
      // file location and there was an error writing, update the stream's file
      // location
      if (currDataHeaderLoc.pageLoc == stream->fileLoc.pageLoc) {
        stream->fileLoc = *wearLevelInfo.storfsCurrLoc;
      }
    } else {
      currDataHeaderLoc = nextDataHeaderLoc;
    }
    prevDataHeaderLoc = *wearLevelInfo.storfsCurrLoc;

    // Increment current iteration number
    currItr++;

    // Increment read file size remainder
    stream->fileRead.fileSizeRem += (wearLevelInfo.sendDataLen - headerLen);
    STORFS_LOGD(TAG, "Read File Size Remainder %ld",
                stream->fileRead.fileSizeRem);

    // Set the append header byte location to 0
    if (appendHeaderByteLoc > 0) {
      appendHeaderByteLoc = 0;
    }
  } while (sendDataItr > 0);

  // Store the updated header into the file information
  if (file_header_store_helper(storfsInst, &stream->fileInfo, stream->fileLoc,
                               "Updated FILE") != STORFS_OK) {
    return STORFS_ERROR;
  }

  // Find and update the next open byte available if the next open byte is
  // currently larger than the file's location
  if (storfsInst->cachedInfo.nextOpenByte <=
      BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc,
                           storfsInst)) {
    currDataHeaderLoc.pageLoc =
        LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) - 1;
    find_update_next_open_byte(storfsInst, currDataHeaderLoc);
  } else {
    // Update the root with current values possibly overwritten when using
    // wear-levelling
    update_root(storfsInst);
  }

  if (stream->fileFlags & STORFS_FILE_REWIND_FLAG) {
    STORFS_LOGD(TAG, "Rewound file has been written");
    stream->fileFlags &= ~(STORFS_FILE_REWIND_FLAG);
  }

  file_info_display_helper(stream->fileInfo);

  return STORFS_OK;
}
