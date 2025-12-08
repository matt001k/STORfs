#include "storfs.h"
#include "validate.h"
#include <string.h>


storfs_err_t storfs_validate_name(const char *name) {
    if (!name) {
        return STORFS_ERR_NULL_POINTER;
    }
    
    size_t len = strlen(name);
    
    // Check minimum length (must be at least 1)
    if (len == 0) {
        return STORFS_ERR_INVALID_PARAM;
    }
    
    // Check maximum length
    if (len > STORFS_MAX_FILE_NAME) {
        return STORFS_ERR_NAME_TOO_LONG;
    }
    
    // Check for invalid characters in filename
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c < 32 || c == 127) {
            return STORFS_ERR_INVALID_PARAM;
        }
    }
    
    return STORFS_OK;
}

storfs_err_t storfs_validate_page_location(const storfs_t *fs, storfs_page_t page) {
    if (!fs) {
        return STORFS_ERR_NULL_POINTER;
    }
    
    if (page >= fs->pageCount) {
        return STORFS_ERR_INVALID_PARAM;
    }
    
    return STORFS_OK;
}

storfs_err_t storfs_validate_byte_location(const storfs_t *fs, storfs_byte_t byte) {
    if (!fs) {
        return STORFS_ERR_NULL_POINTER;
    }
    
    if (byte >= fs->pageSize) {
        return STORFS_ERR_INVALID_PARAM;
    }
    
    return STORFS_OK;
}


