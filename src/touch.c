#include "core.h"
#include "storfs.h"

storfs_err_t storfs_touch(storfs_t *storfsInst, char *pathToFile) {
  STORFS_LOGI(TAG, "Making File at %s", pathToFile);

  return file_handling_helper(storfsInst, (storfs_name_t *)pathToFile,
                              FILE_CREATE, NULL);
}
