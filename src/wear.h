#ifndef __STORFS_WEAR_H__
#define __STORFS_WEAR_H__

#include "storfs.h"

typedef struct {
  uint8_t             *sendBuf;
  storfs_loc_t         storfsOrigLoc;
  storfs_loc_t        *storfsCurrLoc;
  storfs_loc_t         storfsPrevLoc;
  uint32_t             sendDataLen;
  uint32_t             headerLen;
  storfs_file_header_t storfsInfo;
  storfs_loc_t         storfsInfoLoc;
  storfs_file_flags_t  storfsFlags;
} wear_level_t;

/** @brief Functions used for wear levelling */
storfs_err_t write_wear_level_helper(storfs_t     *storfsInst,
                                     wear_level_t *wearLevelInfo);

#endif
