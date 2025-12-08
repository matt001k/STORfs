#ifndef __STORFS_VALIDATE_H__
#define __STORFS_VALIDATE_H__

#include "storfs.h"

#define STORFS_VALIDATE_MOUNTED(fs) \
    do { if (!(fs)->mounted) return STORFS_ERR_NOT_MOUNTED; } while(0)

// Path validation function
storfs_err_t storfs_validate_path(const char *path);

// Name validation function  
storfs_err_t storfs_validate_name(const char *name);

// Page/byte validation
storfs_err_t storfs_validate_page_location(const storfs_t *fs, storfs_page_t page);
storfs_err_t storfs_validate_byte_location(const storfs_t *fs, storfs_byte_t byte);

#endif
