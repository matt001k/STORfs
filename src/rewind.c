#include "core.h"
#include "storfs.h"

storfs_err_t storfs_rewind(storfs_t *storfsInst, STORFS_FILE *stream) {
  if(!storfsInst || !stream || stream->fileFlags == STORFS_FILE_DELETED_FLAG) {
    STORFS_LOGE(TAG, "Error in opening the current file stream");
    return STORFS_ERR_NULL_POINTER;
  }

  STORFS_LOGI(TAG,
              "Rewinding file %s to original location",
              stream->fileInfo.fileName);

  // Set read pointer location
  stream->fileRead.readLocPtr.pageLoc = stream->fileLoc.pageLoc;
  stream->fileRead.readLocPtr.byteLoc = STORFS_HEADER_TOTAL_SIZE;
  // Set read file size remainder
  stream->fileRead.fileSizeRem =
      stream->fileInfo.fileSize - STORFS_HEADER_TOTAL_SIZE -
      (stream->fileInfo.fileSize / storfsInst->pageSize *
       STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

  STORFS_LOGD(TAG, "File size remainder %ld", stream->fileRead.fileSizeRem);

  // Set rewind flag
  stream->fileFlags |= STORFS_FILE_REWIND_FLAG;

  return STORFS_OK;
}
