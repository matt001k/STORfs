#include "core.h"
#include "storfs.h"

static storfs_err_t
directory_delete_helper(storfs_t *storfsInst, storfs_loc_t rmParentLoc,
                        storfs_file_header_t rmParentHeader) {
  STORFS_LOGI(TAG, "Deleting directory and all of it's containing files");

  // Remove the directory
  if (file_delete_helper(storfsInst, rmParentLoc, rmParentHeader) !=
      STORFS_OK) {
    return STORFS_ERROR;
  }

  // If the parent header has a child location, ensure the children get deleted
  if (rmParentHeader.childLocation != 0x00) {
    storfs_file_header_t rmChildHeader;
    storfs_loc_t rmChildFileLoc;
    rmChildFileLoc.pageLoc =
        LOCATION_TO_PAGE(rmParentHeader.childLocation, storfsInst);
    rmChildFileLoc.byteLoc =
        LOCATION_TO_BYTE(rmParentHeader.childLocation, storfsInst);

    file_header_store_helper(storfsInst, &rmChildHeader, rmChildFileLoc,
                             "Remove");
    while (1) {
      // If a directory needs to be deleted, iterate through this
      if (rmChildHeader.childLocation != 0x00) {
        if (directory_delete_helper(storfsInst, rmChildFileLoc,
                                    rmChildHeader) != STORFS_OK) {
          return STORFS_ERROR;
        }
        rmChildFileLoc.pageLoc =
            LOCATION_TO_PAGE(rmChildHeader.childLocation, storfsInst);
        rmChildFileLoc.byteLoc =
            LOCATION_TO_BYTE(rmChildHeader.childLocation, storfsInst);
      } else {
        if (file_delete_helper(storfsInst, rmChildFileLoc, rmChildHeader) !=
            STORFS_OK) {
          return STORFS_ERROR;
        }
        rmChildFileLoc.pageLoc =
            LOCATION_TO_PAGE(rmChildHeader.siblingLocation, storfsInst);
        rmChildFileLoc.byteLoc =
            LOCATION_TO_BYTE(rmChildHeader.siblingLocation, storfsInst);
      }

      // If no siblings, this will be the last file within the directory
      if (rmChildHeader.siblingLocation == 0x00) {
        break;
      }
      file_header_store_helper(storfsInst, &rmChildHeader, rmChildFileLoc,
                               "Remove");
    }
  }

  return STORFS_OK;
}

storfs_err_t storfs_rm(storfs_t *storfsInst, char *pathToFile,
                       STORFS_FILE *stream) {
  // Error Checking
  if (!storfsInst || !pathToFile) {
    return STORFS_ERR_NULL_POINTER;
  }

  STORFS_FILE rmStream;
  storfs_file_header_t storfsPreviousHeader;
  STORFS_LOGI(TAG, "Removing file at %s", pathToFile);

  // Open the file again in order to find the current parent/sibling/children
  if (file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_OPEN,
                           &rmStream) != STORFS_OK) {
    return STORFS_ERROR;
  }

  // If the item to delete is a file or directory
  if ((rmStream.fileInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
      STORFS_INFO_REG_FILE_TYPE_FILE) {
    if (stream != NULL) {
      // Set the stream flag to Deleted so it may not be used again until
      // opened/created
      stream->fileFlags = STORFS_FILE_DELETED_FLAG;
    }

    if (file_delete_helper(storfsInst, rmStream.fileLoc, rmStream.fileInfo) !=
        STORFS_OK) {
      return STORFS_ERROR;
    }

  } else {
    if (directory_delete_helper(storfsInst, rmStream.fileLoc,
                                rmStream.fileInfo) != STORFS_OK) {
      return STORFS_ERROR;
    }
  }

  // Store the previous header and manipulate it
  file_header_store_helper(storfsInst, &storfsPreviousHeader,
                           rmStream.filePrevLoc, "Previous");

  // Update the child or sibling directory of the previous file location
  if (rmStream.filePrevLoc.pageLoc == storfsInst->firstPageLoc) {
    storfsInst->cachedInfo.rootHeaderInfo[0].childLocation =
        rmStream.fileInfo.siblingLocation;
    storfsInst->cachedInfo.rootHeaderInfo[1].childLocation =
        rmStream.fileInfo.siblingLocation;
  } else if (rmStream.filePrevFlags == STORFS_FILE_PARENT_FLAG) {
    storfsPreviousHeader.childLocation = rmStream.fileInfo.siblingLocation;
    // Remove the header from storage so it may be re-written
    if (storfsInst->erase(storfsInst, rmStream.filePrevLoc.pageLoc) !=
        STORFS_OK) {
      return STORFS_ERROR;
    }
    if (file_header_create_helper(storfsInst, &storfsPreviousHeader,
                                  rmStream.filePrevLoc, "") != STORFS_OK) {
      return STORFS_ERROR;
    }
  } else {
    // If the current file being deleted has a sibling, update the previous
    // register's sibling with the current registers sibling
    storfsPreviousHeader.siblingLocation = rmStream.fileInfo.siblingLocation;

    // If the previous file is a directory simply update the header, if not read
    // in the data of the original file page and update the page with a new
    // header
    if ((storfsPreviousHeader.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) ==
        STORFS_INFO_REG_FILE_TYPE_DIRECTORY) {
      // Remove the header from storage so it may be re-written
      if (storfsInst->erase(storfsInst, rmStream.filePrevLoc.pageLoc) !=
          STORFS_OK) {
        return STORFS_ERROR;
      }
      if (file_header_create_helper(storfsInst, &storfsPreviousHeader,
                                    rmStream.filePrevLoc, "") != STORFS_OK) {
        return STORFS_ERROR;
      }
    } else {
      uint8_t siblingBuf[storfsInst->pageSize];
      uint8_t updatedHeader[STORFS_HEADER_TOTAL_SIZE];

      STORFS_LOGD(TAG,
                  "Updating Previous File Sibling Location at the file's "
                  "initial location at %ld%ld, %d",
                  (uint32_t)(rmStream.filePrevLoc.pageLoc >> 32),
                  (uint32_t)(rmStream.filePrevLoc.pageLoc), 0);

      if (storfsInst->read(storfsInst, rmStream.fileLoc.pageLoc,
                           STORFS_HEADER_TOTAL_SIZE, siblingBuf,
                           (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) !=
          STORFS_OK) {
        return STORFS_ERR_READ_FAILED;
      }

      // Remove the header from storage so it may be re-written
      if (storfsInst->erase(storfsInst, rmStream.filePrevLoc.pageLoc) !=
          STORFS_OK) {
        return STORFS_ERROR;
      }

      info_to_buf(updatedHeader, &storfsPreviousHeader);
      for (storfs_size_t i = 0; i < storfsInst->pageSize; i++) {
        if (i < STORFS_HEADER_TOTAL_SIZE) {
          siblingBuf[i] = updatedHeader[i];
        } else {
          siblingBuf[i] = siblingBuf[i - STORFS_HEADER_TOTAL_SIZE];
        }
      }
      if (storfsInst->write(storfsInst, rmStream.filePrevLoc.pageLoc, 0,
                            siblingBuf, storfsInst->pageSize) != STORFS_OK) {
        return STORFS_ERR_WRITE_FAILED;
      }
    }
  }

  // Update the next open byte to the file that was deleted if the next open
  // byte is currently larger than the files location
  if (storfsInst->cachedInfo.nextOpenByte >=
      BYTEPAGE_TO_LOCATION(rmStream.fileLoc.byteLoc, rmStream.fileLoc.pageLoc,
                           storfsInst)) {
    update_root_next_open_byte(
        storfsInst, BYTEPAGE_TO_LOCATION(rmStream.fileLoc.byteLoc,
                                         rmStream.fileLoc.pageLoc, storfsInst));
  }

  return STORFS_OK;
}
