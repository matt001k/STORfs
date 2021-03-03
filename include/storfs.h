/*
* Copyright 2020 KrauseGLOBAL Solutions, LLC
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef __STORFS_H
#define __STORFS_H

#include "storfs_config.h"

#include <stdint.h>

/** @brief Maximum file name characters for the header information 
 *  cannot be less than 4 characters*/ 
#ifndef STORFS_MAX_FILE_NAME
    #define STORFS_MAX_FILE_NAME  32
#elif STORFS_MAX_FILE_NAME < 4
    #define STORFS_MAX_FILE_NAME  4
#endif

/** @brief STORFS_LOGging defines for serial output/display functionality */
#ifndef STORFS_NO_LOG
    #ifndef STORFS_LOGI
        #define STORFS_LOGI(TAG, fmt, ...) \
            printf("| I |" fmt "\n", ##__VA_ARGS__)
    #endif
    #ifndef STORFS_LOGD
        #define STORFS_LOGD(TAG, fmt, ...) \
            printf("| D |" fmt "\n", ##__VA_ARGS__)
    #endif
    #ifndef STORFS_LOGW
        #define STORFS_LOGW(TAG, fmt, ...) \
            printf("| W |" fmt "\n",  ##__VA_ARGS__)
    #endif
    #ifndef STORFS_LOGE
        #define STORFS_LOGE(TAG, fmt, ...) \
            printf("| E |" fmt "\n", ##__VA_ARGS__)
    #endif
#else
    #define STORFS_LOGI(TAG, fmt, ...)
    #define STORFS_LOGD(TAG, fmt, ...)  
    #define STORFS_LOGW(TAG, fmt, ...) 
    #define STORFS_LOGE(TAG, fmt, ...) 
#endif

#define STORFS_INFO_REG_SIZE                                1
#define STORFS_CHILD_DIR_REG_SIZE                           8
#define STORFS_SIBLING_DIR_SIZE                             8
#define STORFS_RESERVED_SIZE                                2
#define STORFS_FRAGMENT_LOC_SIZE                            8
#define STORFS_FILE_SIZE                                    4
#define STORFS_CRC_SIZE                                     2
#define STORFS_HEADER_TOTAL_SIZE                            (STORFS_INFO_REG_SIZE + STORFS_CHILD_DIR_REG_SIZE + \
                                                            STORFS_SIBLING_DIR_SIZE + STORFS_RESERVED_SIZE + \
                                                            STORFS_FRAGMENT_LOC_SIZE + STORFS_FILE_SIZE + \
                                                            STORFS_CRC_SIZE + STORFS_MAX_FILE_NAME)
#define STORFS_FRAGMENT_HEADER_TOTAL_SIZE                   (STORFS_INFO_REG_SIZE + STORFS_RESERVED_SIZE + \
                                                            STORFS_FRAGMENT_LOC_SIZE + STORFS_CRC_SIZE)

/** @brief File Info Register Bit Definitions */
#define STORFS_INFO_REG_NOT_FRAGMENT_BIT                    (0X1 << 7)
#define STORFS_INFO_REG_BLOCK_SIGN_EMPTY                    (0X3 << 5)
#define STORFS_INFO_REG_BLOCK_SIGN_PART_FULL                (0X2 << 5)
#define STORFS_INFO_REG_BLOCK_SIGN_FULL                     (0X1 << 5)
#define STORFS_INFO_REG_FILE_TYPE_FILE                      (0X3 << 2)
#define STORFS_INFO_REG_FILE_TYPE_DIRECTORY                 (0X2 << 2)
#define STORFS_INFO_REG_FILE_TYPE_ROOT                      (0X1 << 2)
#define STORFS_INFO_REG_FILE_TYPE_FILE_FRAGMENT             (0X0 << 2)


/** @brief Alias for size in bytes of items */ 
typedef uint64_t storfs_size_t;

/** @brief Describes the page location */ 
typedef uint64_t storfs_page_t;

/** @brief Describes the byte location */ 
typedef uint32_t storfs_byte_t;

/** @brief Filesize alias for filesize register */ 
typedef uint32_t storfs_file_size_t;

/** @brief Used to store the name of a file */ 
typedef uint8_t storfs_name_t;

/** @brief Used to store the information of a file */ 
typedef uint8_t storfs_file_info_t;

/** @brief Used for CRC */ 
typedef uint16_t storfs_crc_t;

/** @brief Error handling enum */ 
typedef enum {
    STORFS_OK = 0x0UL,
    STORFS_ERROR,
    STORFS_WRITE_FAILED,
    STORFS_READ_FAILED,
    STORFS_MEMORY_DISCREPENCY,
    STORFS_CRC_ERR,
} storfs_err_t;

/** @brief Location struct for the specific page and byte in that page to read/write to/from */ 
typedef struct {
    storfs_page_t pageLoc;
    storfs_byte_t byteLoc;
} storfs_loc_t;

/** @brief File header information struct */ 
typedef struct {
    storfs_name_t fileName[STORFS_MAX_FILE_NAME];
    storfs_file_info_t fileInfo;
    storfs_page_t childLocation;
    storfs_page_t siblingLocation;
    uint16_t reserved;
    storfs_page_t fragmentLocation;
    storfs_file_size_t fileSize;
    storfs_crc_t crc;
} storfs_file_header_t;

/** @brief "Cache" for items in the current filesystem instance */ 
typedef struct 
{
    storfs_file_header_t rootHeaderInfo[2];
    storfs_page_t nextOpenByte;
    storfs_loc_t rootLocation[2];
} storfs_cached_info_t;

/** @brief Filesystem Configuration */
typedef struct storfs{
    //Instance to the memory interface to be used by the filesystem
    void *memInst;

    /**
     * @brief       Read Callback
     *              Callback to read data from a page with a specific byte offset
     *
     * @attention   If the offset and the size of the data read is more than the page size
     *              the data will start reading from the next page
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       page        Page number to read data from
     * @param       byte        Byte offset within the page
     * @param       buffer      Buffer to store the data read from
     * @param       size        Total size of the data to be read
     * @return      STORFS_OK   Succeed
     */
    storfs_err_t (*read)(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size);

    /**
     * @brief       Write Callback
     *              Callback to write data to a page with a specific byte offset
     *
     * @attention   If the offset and the size of the data written is more than the page size
     *              the data will start writing to the next page
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       page        Page number to write data from
     * @param       byte        Byte offset within the page
     * @param       buffer      Buffer to send data to the memory device
     * @param       size        Size of the data to be sent
     * @return      STORFS_OK   Succeed
     */
    storfs_err_t (*write)(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size);

    /**
     * @brief       Erase Callback
     *              Callback to Erase data for a page
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       page        Page number to write data from
     * @return      STORFS_OK   Succeed
     */
    storfs_err_t (*erase)(const struct storfs *storfsInst, storfs_page_t page);

    /**
     * @brief       Sync Callback
     *              Callback to sync data, ensure that the memory is ready to receive the next data
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @return      STORFS_OK   Succeed
     */
    storfs_err_t (*sync)(const struct storfs *storfsInst);

#ifdef STORFS_USE_CRC
    /**
     * @brief       CRC Callback
     *              Callback used to determine if a CRC function will be defined by the user
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       buffer      Buffer of data to determine the crc remainder
     * @param       size        Length in bytes of the buffer to compute the crc
     * @return      STORFS_OK   Succeed
    */
    storfs_err_t (*crc)(const struct storfs *storfsInst, const uint8_t *buffer, storfs_size_t size);
#endif

#ifdef STORFS_THREADSAFE
    int (*lock)(const struct storfs_t *storfsInst);

    int (*unlock)(const struct storfs_t *storfsInst);
#endif

    /** @brief Location of the first page and byte within that page in memory for the directory to exist within */
    storfs_size_t firstPageLoc;
    storfs_size_t firstByteLoc;

    /** @brief Size of the page/block/sector/section in bytes within the storage device typically 512 Bytes
    This should be the lowest section available to write to a device */ 
    storfs_size_t pageSize;

    /** @brief Number of erasable page/block/sector/section in bytes within the storage device typically 512 Bytes */
    storfs_size_t pageCount;
    
    /** @brief Information cached for use throughout the instance */
    storfs_cached_info_t cachedInfo;

} storfs_t;


 /**
     * @brief       Mount File System
     *              Used to mount the file system in order to correctly read/write files
     *
     * @attention   When first calling storfs_mount, a name must be used for the partition 
     *              ex: C:
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       partName    Name of the root partition
     * @return      STORFS_OK   Succeed
     */
storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName);

/**
     * @brief       mkdir
     *              Used to make a directory within the file system
     *
     * @attention   A directory cannot have a file extension
     * @attention   Multiple directories may be made at once
     * @attention   The pathToDir must be a full path from the root to the current directory
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       pathToDir   Path to the directory from the root partition
     * @return      STORFS_OK   Succeed
*/
storfs_err_t storfs_mkdir(storfs_t *storfsInst, char *pathToDir);

/**
     * @brief       touch
     *              Used to make a file within the file system
     *
     * @attention   A single file may only be made at once
     * @attention   The pathToDir must be a full path from the root to the current directory
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       pathToFile  Path to the file from the root partition
     * @return      STORFS_OK   Succeed
*/
storfs_err_t storfs_touch(storfs_t *storfsInst, char *pathToFile);

/** @brief Flags when opening up a file */ 
typedef uint8_t storfs_file_flags_t;

/** @brief FILE struct for saving data to when opening up a file */ 
typedef struct storfs_fopen_file_info{
    storfs_file_header_t fileInfo;
    storfs_loc_t fileLoc;
    storfs_file_flags_t fileFlags;
    storfs_loc_t filePrevLoc;
    storfs_file_flags_t filePrevFlags;
} STORFS_FILE;

/**
     * @brief       fopen
     *              Used to make/open a file within the file system
     *
     * @attention   A single file may only be made at once
     * @attention   The pathToDir must be a full path from the root to the current directory
     * @attention   mode flags include:
     *                  - w: write only and truncate existing file
     *                  - w+: read/write and truncate existing file
     *                  - r: read only
     *                  - r+: read/write and truncate the existing file
     *                  - a: write only and append existing file
     *                  - a+: read/write and append existing file
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       pathToFile  Path to the file from the root partition
     * @param       mode        Mode to open the file in
     * @param       stream      File to save the file information from the function
     * @return      STORFS_OK   Succeed
*/
storfs_err_t storfs_fopen(storfs_t *storfsInst, char *pathToFile, const char * mode, STORFS_FILE *stream);

/**
     * @brief       fputs
     *              Used to write to a file
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       str         data to write to the file
     * @param       n           length of data to write to the file
     * @param       stream      File to write to
     * @return      STORFS_OK   Succeed
*/
storfs_err_t storfs_fputs(storfs_t *storfsInst, const char *str, const int n, STORFS_FILE *stream);

/**
     * @brief       fgets
     *              Used to read a file
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       str         data to read from the file
     * @param       n           length of data to read from the file
     * @param       stream      File to read from
     * @return      STORFS_OK   Succeed
*/
storfs_err_t storfs_fgets(storfs_t *storfsInst, char *str, int n, STORFS_FILE *stream);

/**
     * @brief       rm
     *              Used to remove a file
     * 
     * @attention   To remove a directory and all of its contents stream must be NULL
     *              
     * @param       storfsInst  Instance used for the STORfs
     * @param       pathToFile  Path to the file from the root partition
     * @param       stream      File to delete
     * @return      STORFS_OK   Succeed
*/
storfs_err_t storfs_rm(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *stream);


storfs_err_t storfs_display_header(storfs_t *storfsInst, storfs_loc_t loc);

 storfs_err_t wear_level_activate(storfs_t* storfsInst, storfs_file_header_t storfsInfo, storfs_loc_t *storfsCurrLoc, storfs_loc_t storfsPrevLoc, uint32_t sendDataLen);

#endif