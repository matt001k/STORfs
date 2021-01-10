/*
 * Author: Matthew Krause
 * 
 * Description: File is used to set up SPI for STM32 Microcontrollers
 *              Along with functionality to allow for configuring the device to the needs of  
 *              the application
 * 
 * Revision History:
 *      Name:               Date:           Description:            Revision Number:
 *      Matthew Krause      12/17/20        File creation           A
 * 
 * Copyright (c) 2020 - KrauseGLOBAL
 */
#ifndef __STORFS_H
#define __STORFS_H

#include "storfs_config.h"

#include <stdint.h>

/** @brief Maximum file name characters for the header information 
 *  cannot be less than 4 characters*/ 
#ifndef STORFS_MAX_FILE_NAME
    #define STORFS_MAX_FILE_NAME  32
#elif STORFS_MAX_FILE_NAME < 2
    #define STORFS_MAX_FILE_NAME  4
#endif

/** @brief Logging defines for serial output/display functionality */
#ifndef STORFS_NO_LOG
    #ifndef LOGI
        #define LOGI(TAG, fmt, ...) \
            printf("| I |" fmt "\n", ##__VA_ARGS__)
    #endif
    #ifndef LOGD
        #define LOGD(TAG, fmt, ...) \
            printf("| D |" fmt "\n", ##__VA_ARGS__)
    #endif
    #ifndef LOGW
        #define LOGW(TAG, fmt, ...) \
            printf("| W |" fmt "\n",  ##__VA_ARGS__)
    #endif
    #ifndef LOGE
        #define LOGE(TAG, fmt, ...) \
            printf("| E |" fmt "\n", ##__VA_ARGS__)
    #endif
#else
    #define LOGI(TAG, fmt, ...)
    #define LOGD(TAG, fmt, ...)  
    #define LOGW(TAG, fmt, ...) 
    #define LOGE(TAG, fmt, ...) 
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

#define STORFS_INFO_REG_NOT_FRAGMENT_BIT                        (0X1 << 7)
#define STORFS_INFO_REG_BLOCK_SIGN_EMPTY                    (0X3 << 5)
#define STORFS_INFO_REG_BLOCK_SIGN_PART_FULL                (0X2 << 5)
#define STORFS_INFO_REG_BLOCK_SIGN_FULL                     (0X1 << 5)
#define STORFS_INFO_REG_FILE_TYPE_FILE                      (0X3 << 2)
#define STORFS_INFO_REG_FILE_TYPE_DIRECTORY                 (0X2 << 2)
#define STORFS_INFO_REG_FILE_TYPE_ROOT                      (0X1 << 2)


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
    storfs_err_t (*prog)(const struct storfs *storfsInst, storfs_page_t page,
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

storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName);

storfs_err_t storfs_mkdir(storfs_t *storfsInst, char *pathToDir);
storfs_err_t storfs_touch(storfs_t *storfsInst, char *pathToFile);

typedef uint8_t storfs_file_flags_t;

typedef struct storfs_fopen_file_info{
    storfs_file_header_t fileInfo;
    storfs_loc_t fileLoc;
    storfs_file_flags_t fileFlags;
    storfs_loc_t filePrevLoc;
    storfs_file_flags_t filePrevFlags;
} STORFS_FILE;

STORFS_FILE storfs_fopen(storfs_t *storfsInst, char *pathToFile, const char * mode);

storfs_err_t storfs_fputs(storfs_t *storfsInst, const char *str, const int n, STORFS_FILE *stream);
storfs_err_t storfs_fgets(storfs_t *storfsInst, char *str, int n, STORFS_FILE *stream);
storfs_err_t storfs_rm(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *stream);


void storfs_display_header(storfs_t *storfsInst, storfs_loc_t loc);


#endif