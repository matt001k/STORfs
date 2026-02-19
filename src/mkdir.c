#include "core.h"

storfs_err_t storfs_mkdir(storfs_t *storfsInst, char *pathToDir) {
  if(!storfsInst || !pathToDir) {
    return STORFS_ERR_NULL_POINTER;
  }

  STORFS_LOGI(TAG, "Making Directory at %s", pathToDir);

  return file_handling_helper(storfsInst,
                              (storfs_name_t *)pathToDir,
                              DIR_CREATE,
                              NULL);
}
