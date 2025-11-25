#ifndef __STORFS_CORE_H__
#define __STORFS_CORE_H__

#include "storfs.h"

#define LOCATION_TO_PAGE(location, storfsInst)              (location / storfsInst->pageSize)
#define LOCATION_TO_BYTE(location, storfsInst)              ((location + storfsInst->pageSize) % storfsInst->pageSize)
#define BYTEPAGE_TO_LOCATION(byte,page,storfsInst)          ((page * storfsInst->pageSize) + byte)


storfs_err_t find_next_open_byte_helper (storfs_t *storfsInst, storfs_loc_t *storfsLoc);

/** @brief Function to handle opening/creating new files, most important function of STORfs */
typedef enum {
    FILE_WRITE = 0x0UL,
    FILE_READ,
    FILE_CREATE,
    DIR_CREATE,
    FILE_OPEN,
    FILE_APPEND,
} file_action_t;
storfs_err_t file_handling_helper(storfs_t *storfsInst, storfs_name_t *pathToDir, file_action_t actionFlag, void *buff);

/** @brief Header creation/storage/display functions */
storfs_err_t file_header_create_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string);
storfs_err_t file_header_store_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string);
void file_info_display_helper(storfs_file_header_t storfsInfo);
storfs_err_t file_delete_helper(storfs_t *storfsInst, storfs_loc_t storfsLoc, storfs_file_header_t storfsInfo);

storfs_err_t update_root(storfs_t *storfsInst);
storfs_err_t update_root_next_open_byte(storfs_t *storfsInst, storfs_size_t fileLocation);
storfs_err_t find_update_next_open_byte(storfs_t *storfsInst, storfs_loc_t storfsLoc);

void info_to_buf(uint8_t *buf, storfs_file_header_t *storfsInfo);
void buf_to_info(uint8_t *buf, storfs_file_header_t *storfsInfo);

#endif
