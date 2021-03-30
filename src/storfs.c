#include "storfs.h"
#include "storfs_config.h"

#include <string.h>

#define IS_EMPTY_FILE(info)

#define LOCATION_TO_PAGE(location, storfsInst)              (location / storfsInst->pageSize)
#define LOCATION_TO_BYTE(location, storfsInst)              ((location + storfsInst->pageSize) % storfsInst->pageSize)
#define BYTEPAGE_TO_LOCATION(byte,page,storfsInst)          ((page * storfsInst->pageSize) + byte)

#define SET_NULL(ptr)                                       (ptr = NULL)

#define GET_STR_LEN(strLen, str) \
    do { \
      *strLen += 1; \
    } while(str[*(strLen)-1] != '\0');

typedef struct {
    uint8_t                 *sendBuf;
    storfs_loc_t            storfsOrigLoc;
    storfs_loc_t            *storfsCurrLoc;
    storfs_loc_t            storfsPrevLoc;
    uint32_t                sendDataLen;
    uint32_t                headerLen;
    storfs_file_header_t    storfsInfo;
    storfs_loc_t            storfsInfoLoc;
    storfs_file_flags_t     storfsFlags;
} wear_level_t;

/** @brief Wear handling enum */ 
typedef enum {
    WRITE_GOOD = 0x0UL,
    WRITE_BAD,
    WRITE_RELOCATE,
} wear_level_state_t;

typedef enum {
    WEAR_FPUTS = 0x0UL,
    WEAR_CREATE,
} wear_level_enum_t;

typedef enum {
    FILE_WRITE = 0x0UL,
    FILE_READ,
    FILE_CREATE,
    DIR_CREATE,
    FILE_OPEN,
    FILE_APPEND,
} file_action_t;

typedef enum {
    PATH_LAST = 0x0UL,
    PATH_LEFT,
} path_flag_t;

typedef enum {
    FILE_MAIN = 0X0UL,
    FILE_FRAGMENT,
} header_flag_t;

/** @brief Flags used for FILE struct */
#define STORFS_FILE_WRITE_FLAG                  0x00000001
#define STORFS_FILE_READ_FLAG                   0x00000002
#define STORFS_FILE_APPEND_FLAG                 0x00000004
#define STORFS_FILE_PARENT_FLAG                 0x00000008
#define STORFS_FILE_SIBLING_FLAG                0x00000010
#define STORFS_FILE_INIT_HEADER_WRITE           0x00000020
#define STORFS_FILE_HEADER_WRITE                0x00000040
#define STORFS_FILE_WRITE_INIT_FLAG             0x00000080
#define STORFS_FILE_REWIND_FLAG                 0x00000100
#define STORFS_FILE_DELETED_FLAG                0xF1

#ifdef STORFS_USE_CRC
    #define STORFS_CRC_CALC(storfsInst, buf, buflen)    \
        (storfsInst->crc(storfsInst, buf, buflen))
#else
    static uint16_t storfs_crc16(const uint8_t* buf, uint32_t bufLen);
    #define STORFS_CRC_CALC(storfsInst, buf, buflen)    \
        storfs_crc16(buf, buflen)
#endif

#ifndef STORFS_USE_CRC
    #define STORFS_POLYNOMIAL 0x8408
    uint16_t storfs_crc16(const uint8_t* buf, uint32_t bufLen)
    {
        uint8_t i;
        uint32_t data;
        uint16_t crc = 0xffff;
        if (bufLen == 0)
              return (~crc);
        do
        {
              for (i=0, data=(unsigned int)0xff & *buf++; i < 8; i++, data >>= 1)
              {
                    if ((crc & 0x0001) ^ (data & 0x0001))
                          crc = (crc >> 1) ^ STORFS_POLYNOMIAL;
                    else  crc >>= 1;
              }
        } while (--bufLen);
        crc = ~crc;
        data = crc;
        crc = (crc << 8) | (data >> 8 & 0xff);
        return (crc);
    }
        
#endif

static const char* TAG = "STORfs";

/** @brief Used to compare crc code from a file and a buffer */
static storfs_err_t crc_compare(storfs_t *storfsInst, storfs_file_header_t storfsInfo, const uint8_t *buf, uint32_t bufLen);
static storfs_err_t crc_header_check(storfs_t *storfsInst, storfs_loc_t storfsLoc);
static storfs_err_t crc_file_check(storfs_t *storfsInst, storfs_loc_t storfsLoc, uint32_t len);

/** @brief Functions to turn a uint8_t buffer to proper struct used by the file header */
static uint16_t uint8_t_to_uint16_t(uint8_t *buf, uint32_t *index);
static uint32_t uint8_t_to_uint32_t(uint8_t *buf, uint32_t *index);
static uint64_t uint8_t_to_uint64_t(uint8_t *buf, uint32_t *index);
static void buf_to_info(uint8_t *buf, storfs_file_header_t *storfsInfo);

/** @brief Functions to turn file header into a writeable buffer */
static void uint16_t_to_uint8_t(uint8_t *buf, uint16_t uint16Val, uint32_t *index);
static void uint32_t_to_uint8_t(uint8_t *buf, uint32_t uint32Val, uint32_t *index);
static void uint64_t_to_uint8_t(uint8_t *buf, uint64_t uint64Val, uint32_t *index);
static void info_to_buf(uint8_t *buf, storfs_file_header_t *storfsInfo);

/** @brief Header creation/storage/display functions */
static storfs_err_t file_header_create_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string);
static storfs_err_t file_header_store_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string);
static void file_info_display_helper(storfs_file_header_t storfsInfo);

/** @brief Functions to find the next available page to write to and to update the next available byte for the user cache */
static storfs_err_t update_root(storfs_t *storfsInst);
static storfs_err_t update_root_next_open_byte(storfs_t *storfsInst, storfs_size_t fileLocation);
static storfs_err_t find_next_open_byte_helper (storfs_t *storfsInst, storfs_loc_t *storfsLoc);

/** @brief Function to handle opening/creating new files, most important function of STORfs */
static storfs_err_t file_handling_helper(storfs_t *storfsInst, storfs_name_t *pathToDir, file_action_t actionFlag, void *buff);

/** @brief File open helper function for w or w+ modes */
static storfs_err_t fopen_write_flag_helper(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *currentOpenFile);

/** @brief Helper functions to delete directories and files */
static storfs_err_t file_delete_helper(storfs_t *storfsInst, storfs_loc_t storfsLoc, storfs_file_header_t storfsInfo);
static storfs_err_t directory_delete_helper(storfs_t *storfsInst, storfs_loc_t rmParentLoc, storfs_file_header_t rmParentHeader);

/** @brief Functions used for wear levelling */
static storfs_err_t find_prev_file_loc(storfs_t* storfsInst,  storfs_loc_t storfsCurrLoc, storfs_loc_t storfsItrLoc, storfs_loc_t* storfsPrevLoc);
static storfs_err_t wear_level_act(storfs_t* storfsInst, wear_level_t *wearLevelInfo);
static storfs_err_t write_wear_level_helper(storfs_t* storfsInst, wear_level_t *wearLevelInfo);

static storfs_err_t crc_compare(storfs_t *storfsInst, storfs_file_header_t storfsInfo, const uint8_t *buf, uint32_t bufLen)
{
    if(storfsInfo.crc == (STORFS_CRC_CALC(storfsInst, buf, bufLen)))
    {
        STORFS_LOGD(TAG, "CRC Code Correct");
        return STORFS_OK;
    }

    STORFS_LOGE(TAG, "CRC Code Returned Incorrectly");
    return STORFS_CRC_ERR;
}

static storfs_err_t crc_header_check(storfs_t *storfsInst, storfs_loc_t storfsLoc)
{
    storfs_file_header_t storfsInfo;
    uint32_t strLen = 0;

    file_header_store_helper(storfsInst, &storfsInfo, storfsLoc, "CRC Header Check");

    while(storfsInfo.fileName[strLen++] != '\0');

    return crc_compare(storfsInst, storfsInfo, (uint8_t *)storfsInfo.fileName, strLen);
}

static storfs_err_t crc_file_check(storfs_t *storfsInst, storfs_loc_t storfsLoc, uint32_t len)
{
    storfs_file_header_t storfsInfo;
    uint32_t headerLen = STORFS_HEADER_TOTAL_SIZE;
    uint8_t buf[len];

    file_header_store_helper(storfsInst, &storfsInfo, storfsLoc, "CRC File Check");
    if((storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == 0)
    {
        headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
    }

    if(storfsInst->read(storfsInst, storfsLoc.pageLoc, headerLen, buf, len) != STORFS_OK)
    {
        return STORFS_READ_FAILED;
    }

    return crc_compare(storfsInst, storfsInfo, buf, len);
}

static uint16_t uint8_t_to_uint16_t(uint8_t *buf, uint32_t *index)
{
    uint16_t result = 0;
    result |= (uint16_t)buf[*(index)+1];
    result |= (uint16_t)buf[*(index)] << 8;

    *index = *index + 2;

    return result;
}

static uint32_t uint8_t_to_uint32_t(uint8_t *buf, uint32_t *index)
{
    uint32_t result = 0;

    result |= (uint32_t)buf[*(index)+3];
    result |= (uint32_t)buf[*(index)+2] << 8;
    result |= (uint32_t)buf[*(index)+1] << 16;
    result |= (uint32_t)buf[*(index)] << 24;

    *index = *index + 4;

    return result;
}

static uint64_t uint8_t_to_uint64_t(uint8_t *buf, uint32_t *index)
{
    uint64_t result = 0;
    result |= (uint64_t)buf[*(index)+7];
    result |= (uint64_t)buf[*(index)+6] << 8;
    result |= (uint64_t)buf[*(index)+5] << 16;
    result |= (uint64_t)buf[*(index)+4] << 24;
    result |= (uint64_t)buf[*(index)+3] << 32;
    result |= (uint64_t)buf[*(index)+2] << 40;
    result |= (uint64_t)buf[*(index)+1] << 48;
    result |= (uint64_t)buf[*(index)] << 56;

    *index = *index + 8;

    return result;
}

static void buf_to_info(uint8_t *buf, storfs_file_header_t *storfsInfo)
{
    uint32_t i = 0;
    storfsInfo->fileInfo = buf[i++];

    if((storfsInfo->fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == 0)
    {
        storfsInfo->reserved = uint8_t_to_uint16_t(buf, &i);
        storfsInfo->fragmentLocation = uint8_t_to_uint64_t(buf, &i);
        storfsInfo->crc = uint8_t_to_uint16_t(buf, &i);
        //Set all other information equal to zero
        for(int j = 0; j < STORFS_MAX_FILE_NAME; j++)
        {
            storfsInfo->fileName[j] = 0;
        }
        storfsInfo->childLocation = 0;
        storfsInfo->siblingLocation = 0;
        storfsInfo->fileSize = 0;
    }
    else
    {
        while(i < STORFS_MAX_FILE_NAME)
        {
            storfsInfo->fileName[i - STORFS_INFO_REG_SIZE] = buf[i];
            i++;
        }
        storfsInfo->childLocation = uint8_t_to_uint64_t(buf, &i);
        storfsInfo->siblingLocation = uint8_t_to_uint64_t(buf, &i);
        storfsInfo->reserved = uint8_t_to_uint16_t(buf, &i);
        storfsInfo->fragmentLocation = uint8_t_to_uint64_t(buf, &i);
        storfsInfo->fileSize = uint8_t_to_uint32_t(buf, &i);
        storfsInfo->crc = uint8_t_to_uint16_t(buf, &i);
    }
}

static void uint16_t_to_uint8_t(uint8_t *buf, uint16_t uint16Val, uint32_t *index)
{
    buf[*(index)+1] = (uint8_t)(uint16Val);
    buf[*(index)]   = (uint8_t)(uint16Val >> 8);

    *index = *index + 2;
}

static void uint32_t_to_uint8_t(uint8_t *buf, uint32_t uint32Val, uint32_t *index)
{
    buf[*(index)+3] = (uint8_t)(uint32Val);
    buf[*(index)+2] = (uint8_t)(uint32Val >> 8);
    buf[*(index)+1] = (uint8_t)(uint32Val >> 16);
    buf[*(index)]   = (uint8_t)(uint32Val >> 24);

    *index = *index + 4;
}

static void uint64_t_to_uint8_t(uint8_t *buf, uint64_t uint64Val, uint32_t *index)
{
    buf[*(index)+7] = (uint8_t)(uint64Val);
    buf[*(index)+6] = (uint8_t)(uint64Val >> 8);
    buf[*(index)+5] = (uint8_t)(uint64Val >> 16);
    buf[*(index)+4] = (uint8_t)(uint64Val >> 24);
    buf[*(index)+3] = (uint8_t)(uint64Val >> 32);
    buf[*(index)+2] = (uint8_t)(uint64Val >> 40);
    buf[*(index)+1] = (uint8_t)(uint64Val >> 48);
    buf[*(index)]   = (uint8_t)(uint64Val >> 56);

    *index = *index + 8;
}

static void info_to_buf(uint8_t *buf, storfs_file_header_t *storfsInfo)
{
    uint32_t i = 0;
    buf[i] = storfsInfo->fileInfo;
    i++;

    if((storfsInfo->fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == 0)
    {
        uint16_t_to_uint8_t(buf, storfsInfo->reserved, &i);
        uint64_t_to_uint8_t(buf, storfsInfo->fragmentLocation, &i);
        uint16_t_to_uint8_t(buf, storfsInfo->crc, &i);
    }
    else
    {
        while(i < STORFS_MAX_FILE_NAME)
        {
            buf[i] = storfsInfo->fileName[i - STORFS_INFO_REG_SIZE];
            i++;
        }
        uint64_t_to_uint8_t(buf, storfsInfo->childLocation, &i);
        uint64_t_to_uint8_t(buf, storfsInfo->siblingLocation, &i);
        uint16_t_to_uint8_t(buf, storfsInfo->reserved, &i);
        uint64_t_to_uint8_t(buf, storfsInfo->fragmentLocation, &i);
        uint32_t_to_uint8_t(buf, storfsInfo->fileSize, &i);
        uint16_t_to_uint8_t(buf, storfsInfo->crc, &i);
    }
}

#ifdef STORFS_LOG_DISPLAY_HEADER
    static void file_info_display_helper(storfs_file_header_t storfsInfo)
    {
        STORFS_LOGI(TAG, "\t fileInfo %x \r\n \
        fileName %s \r\n \
        childLocation %lx%lx \r\n \
        siblingLocation %lx%lx \r\n \
        reserved %x \r\n \
        fragmentLocation/nextOpenByte %lx%lx \r\n \
        fileSize %lx \r\n \
        crc %x", storfsInfo.fileInfo, storfsInfo.fileName,\
        (uint32_t)(storfsInfo.childLocation >> 32), (uint32_t)storfsInfo.childLocation, \
        (uint32_t)(storfsInfo.siblingLocation >> 32), (uint32_t)storfsInfo.siblingLocation, \
        storfsInfo.reserved, (uint32_t)(storfsInfo.fragmentLocation >> 32), (uint32_t)storfsInfo.fragmentLocation, \
        storfsInfo.fileSize, storfsInfo.crc);
    }
#else
    static void file_info_display_helper(storfs_file_header_t storfsInfo)
    {
        return 0;
    }
#endif

static storfs_err_t file_header_create_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string)
{
    storfs_err_t status = STORFS_OK;
    uint8_t headerBuf[STORFS_HEADER_TOTAL_SIZE];

    //If header to create is overflowing the user defined page size or there is no space left in the storage device return an error
    if(((storfsLoc.byteLoc + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize))
    {
        status = STORFS_WRITE_FAILED;
        goto FUNEND;
    }

    //Turn the header information into a buffer to write to memory
    STORFS_LOGD(TAG, "Writing %s Header at %ld%ld, %ld", string, (uint32_t)(storfsLoc.pageLoc >> 32), \
                (uint32_t)(storfsLoc.pageLoc),  storfsLoc.byteLoc);
    info_to_buf(headerBuf, storfsInfo);
    if(storfsInst->write(storfsInst, storfsLoc.pageLoc, storfsLoc.byteLoc, headerBuf, STORFS_HEADER_TOTAL_SIZE) != STORFS_OK)
    {
        status = STORFS_WRITE_FAILED;
        goto FUNEND;
    }
    status = storfsInst->sync(storfsInst);

    FUNEND:
        return status;
}

static storfs_err_t file_header_store_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string)
{
    storfs_err_t status = STORFS_OK;
    uint8_t headerBuf[STORFS_HEADER_TOTAL_SIZE];

    STORFS_LOGD(TAG, "Storing %s Header at %ld%ld, %ld", string, (uint32_t)(storfsLoc.pageLoc >> 32), \
                (uint32_t)(storfsLoc.pageLoc), storfsLoc.byteLoc);
    if(storfsInst->read(storfsInst, storfsLoc.pageLoc, storfsLoc.byteLoc, headerBuf, STORFS_HEADER_TOTAL_SIZE) != STORFS_OK)
    {
        status = STORFS_READ_FAILED;
        goto FUNEND;
    }
    status = storfsInst->sync(storfsInst);

    buf_to_info(headerBuf, storfsInfo);

    FUNEND:
        return status;
}

static storfs_err_t update_root(storfs_t *storfsInst)
{
    //Update both root registers
    if(storfsInst->erase(storfsInst, storfsInst->cachedInfo.rootLocation[0].pageLoc) != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    file_header_create_helper(storfsInst, &storfsInst->cachedInfo.rootHeaderInfo[0], storfsInst->cachedInfo.rootLocation[0], "Root Header 1");
    if(storfsInst->erase(storfsInst, storfsInst->cachedInfo.rootLocation[1].pageLoc) != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    file_header_create_helper(storfsInst, &storfsInst->cachedInfo.rootHeaderInfo[1], storfsInst->cachedInfo.rootLocation[1], "Root Header 2");

    return STORFS_OK;
}

static storfs_err_t update_root_next_open_byte(storfs_t *storfsInst, storfs_size_t fileLocation)
{
    //Update the cached information with the next open byte
    storfsInst->cachedInfo.nextOpenByte = fileLocation;

    //Update the next open byte info in the cached rootInfo
    storfsInst->cachedInfo.rootHeaderInfo[0].fragmentLocation = fileLocation;
    storfsInst->cachedInfo.rootHeaderInfo[1].fragmentLocation = fileLocation;

    if(update_root(storfsInst) != STORFS_OK)
    {
        return STORFS_ERROR;
    }

    return STORFS_OK;
}

static storfs_err_t find_next_open_byte_helper (storfs_t *storfsInst, storfs_loc_t *storfsLoc)
{
    storfs_file_header_t nextHeaderInfo;
    nextHeaderInfo.fragmentLocation = 0;
    nextHeaderInfo.fileInfo = 0x80;

    //Determine where the next open byte within the system is
    while(nextHeaderInfo.fragmentLocation != 0xFFFFFFFFFFFFFFFF || nextHeaderInfo.siblingLocation != 0xFFFFFFFFFFFFFFFF || \
    nextHeaderInfo.childLocation != 0xFFFFFFFFFFFFFFFF || nextHeaderInfo.fileInfo != 0xFF)
    {
        storfsLoc->pageLoc += 1;
        if(storfsLoc->byteLoc != 0)
        {
            storfsLoc->byteLoc = 0;
        }
        if(file_header_store_helper(storfsInst, &nextHeaderInfo, *storfsLoc, "Next") != STORFS_OK)
        {
            return STORFS_ERROR;
        }
    }

    return STORFS_OK;
}

static storfs_err_t find_update_next_open_byte(storfs_t *storfsInst, storfs_loc_t storfsLoc)
{
    STORFS_LOGD(TAG, "Finding and updating next open byte");
    if(find_next_open_byte_helper(storfsInst, &storfsLoc) != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    
    //Update the next open byte available
    update_root_next_open_byte(storfsInst, BYTEPAGE_TO_LOCATION(0, storfsLoc.pageLoc, storfsInst));

    return STORFS_OK;
}

static storfs_err_t file_handling_helper(storfs_t *storfsInst, storfs_name_t *pathToDir, file_action_t actionFlag, void *buff)
{
    int strLen = 0;                                                         //String length of the path used
    int currStr;                                                            //String length to hold the current file/directory in the path          
    storfs_loc_t currentLocation = storfsInst->cachedInfo.rootLocation[0];  //Location of the current directory to obtain information
    STORFS_FILE previousFile;                                               //Information and location of the previous file
    uint8_t fileSepCnt = 0;                                                 //File separator count to ensure files do no have children
    path_flag_t pathFlag = PATH_LEFT;
    wear_level_t wearLevelInfo;
    uint8_t updatedHeader[STORFS_HEADER_TOTAL_SIZE];

    while(1)
    {
        storfs_name_t currentFileName[STORFS_MAX_FILE_NAME];
        currStr = 0;
        while((pathToDir[strLen] != '/') && (pathToDir[strLen] != '\0'))
        {
            if(pathToDir[strLen] == '.')
            {
                if(actionFlag == DIR_CREATE)
                {
                    STORFS_LOGE(TAG, "Directory name cannot have an extension");
                    return STORFS_ERROR; 
                }
                fileSepCnt++;
            }
            currentFileName[currStr++] = pathToDir[strLen++];
        }
        currentFileName[currStr] = '\0';
        STORFS_LOGD(TAG, "File name %s", currentFileName);

        if(pathToDir[strLen] == '\0')
        {
            pathFlag = PATH_LAST;
        }

        do
        {
            //Store the current file header
            if(file_header_store_helper(storfsInst,  &wearLevelInfo.storfsInfo, currentLocation, "Directory") != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            //Does the current file header name equal to the name in the path?
            if(strcmp((const char *)wearLevelInfo.storfsInfo.fileName, (const char *)currentFileName) == 0)
            {
                //If the filename is matched it is a parent directory, update the previous file information with the current
                STORFS_LOGD(TAG, "File name matched: %s", currentFileName);

                if(pathFlag == PATH_LAST)
                {
                    break;
                }

                //If there is no child location update the child's location to the next open byte
                if(wearLevelInfo.storfsInfo.childLocation == 0x0)
                {
                    wearLevelInfo.storfsInfo.childLocation = storfsInst->cachedInfo.nextOpenByte;
                }

                previousFile.fileLoc = currentLocation;
                previousFile.filePrevLoc = currentLocation;
                previousFile.fileInfo = wearLevelInfo.storfsInfo;
                previousFile.filePrevFlags = STORFS_FILE_PARENT_FLAG;

                //Continue to search the child's location if it is not the last line throughout the path
                currentLocation.pageLoc = LOCATION_TO_PAGE(wearLevelInfo.storfsInfo.childLocation, storfsInst);
                currentLocation.byteLoc = LOCATION_TO_BYTE(wearLevelInfo.storfsInfo.childLocation, storfsInst);
            }
            else if (wearLevelInfo.storfsInfo.siblingLocation != 0xFFFFFFFFFFFFFFFF)
            {
                //If there is no sibling location update the sibling's location to the next open byte
                if(wearLevelInfo.storfsInfo.siblingLocation == 0x0)
                {
                    wearLevelInfo.storfsInfo.siblingLocation = storfsInst->cachedInfo.nextOpenByte;
                }

                //If the filename is not matched and the sibling location exists, it is a sibling directory
                //Update the previous file information with the current
                STORFS_LOGD(TAG, "Name not matched, searching siblings");
                previousFile.fileLoc = currentLocation;
                previousFile.filePrevLoc = currentLocation;
                previousFile.filePrevFlags = STORFS_FILE_SIBLING_FLAG;
                previousFile.fileInfo = wearLevelInfo.storfsInfo;

                //Continue to search the siblings's location
                currentLocation.pageLoc = LOCATION_TO_PAGE(wearLevelInfo.storfsInfo.siblingLocation, storfsInst);
                currentLocation.byteLoc = LOCATION_TO_BYTE(wearLevelInfo.storfsInfo.siblingLocation, storfsInst);
            }
            else
            {
                STORFS_LOGD(TAG, "Name not matched, and no siblings, creating file/directory at next open location");

                //Error is next write is larger than the page count
                if(LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) >= storfsInst->pageCount)
                {
                    STORFS_LOGE(TAG, "Cannot write any more data to the file system");
                }

                //Files cannot be children of other files
                if(fileSepCnt > 1)
                {
                    STORFS_LOGE(TAG, "File/directory cannot be a child of another file");
                    return STORFS_ERROR;
                }

                //Set information for the header of the current file directory
                for(int i = 0; i <= currStr; i++)
                {
                    wearLevelInfo.storfsInfo.fileName[i] = currentFileName[i];
                }
                wearLevelInfo.storfsInfo.reserved = 0xFFFF;
                wearLevelInfo.storfsInfo.fileSize = STORFS_HEADER_TOTAL_SIZE;

                //Set the current directory fragment, sibling and child location registers to zero
                //These locations will never be zero as long as the file system exists, it is a safe value to set these
                wearLevelInfo.storfsInfo.siblingLocation = 0;
                wearLevelInfo.storfsInfo.childLocation = 0;
                wearLevelInfo.storfsInfo.fragmentLocation = 0;

                //Compute CRC of the filename given
                wearLevelInfo.storfsInfo.crc = STORFS_CRC_CALC(storfsInst, wearLevelInfo.storfsInfo.fileName, (currStr+1));

                //If the file size will be the size of the page size, ensure the file info sets the information for the file to full
                if(actionFlag == DIR_CREATE)
                {
                    wearLevelInfo.storfsInfo.fileInfo = STORFS_INFO_REG_FILE_TYPE_DIRECTORY | STORFS_INFO_REG_BLOCK_SIGN_FULL;
                }
                else
                {
                    wearLevelInfo.storfsInfo.fileInfo = STORFS_INFO_REG_FILE_TYPE_FILE | STORFS_INFO_REG_BLOCK_SIGN_PART_FULL;
                }

                info_to_buf(updatedHeader, &wearLevelInfo.storfsInfo);
                wearLevelInfo.sendBuf = updatedHeader;
                wearLevelInfo.headerLen = STORFS_HEADER_TOTAL_SIZE;
                wearLevelInfo.sendDataLen = STORFS_HEADER_TOTAL_SIZE;
                wearLevelInfo.storfsCurrLoc = &currentLocation;
                wearLevelInfo.storfsInfoLoc = currentLocation;
                wearLevelInfo.storfsOrigLoc = currentLocation;
                wearLevelInfo.storfsPrevLoc = previousFile.fileLoc;
                wearLevelInfo.storfsFlags = STORFS_FILE_INIT_HEADER_WRITE | previousFile.filePrevFlags;

                //Write the new header to the needed location in flash
                write_wear_level_helper(storfsInst, &wearLevelInfo);

                //Display the newly created file information
                file_info_display_helper(wearLevelInfo.storfsInfo);

                //Determine the next open byte for the cache and update root header if needed
                if(find_update_next_open_byte(storfsInst, currentLocation) != STORFS_OK)
                {
                    return STORFS_ERROR;
                }
                break;
            }
        } while(previousFile.filePrevFlags == STORFS_FILE_SIBLING_FLAG);

        if(pathFlag == PATH_LAST)
        {
            break;
        }

        strLen += 1;
    }

    //If action is to open the file and store the memory location to the buffer passed in
    if(actionFlag == FILE_OPEN)
    {
        //If the file was just created and file action is FILE_OPEN ensure that the current location is used as the previous location
        previousFile.fileLoc = currentLocation;
        previousFile.fileInfo = wearLevelInfo.storfsInfo;
        *(STORFS_FILE *)buff = previousFile;
    }

    return STORFS_OK;
}

static storfs_err_t fopen_write_flag_helper(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *currentOpenFile)
{
    STORFS_FILE newOpenFile = *currentOpenFile;
    uint32_t strLen = 0;

    //Remove the file since it exists 
    if(file_delete_helper(storfsInst, currentOpenFile->fileLoc, currentOpenFile->fileInfo) != STORFS_OK)
    {
        STORFS_LOGE(TAG, "Cannot delete the old file");
        return STORFS_ERROR;
    }
    
    //Recreate file header
    currentOpenFile->fileInfo.fileSize = STORFS_HEADER_TOTAL_SIZE;
    currentOpenFile->fileInfo.fragmentLocation = 0x00;
    while(currentOpenFile->fileInfo.fileName[strLen++] != '\0');
    currentOpenFile->fileInfo.crc = STORFS_CRC_CALC(storfsInst, currentOpenFile->fileInfo.fileName, strLen);
    while(1)
    {
        if(file_header_create_helper(storfsInst, &currentOpenFile->fileInfo, currentOpenFile->fileLoc, "Deleting old file and opening new") != STORFS_OK)
        {
            STORFS_LOGE(TAG, "Cannot create the old file");
            return STORFS_ERROR;
        }
        if(crc_header_check(storfsInst, currentOpenFile->fileLoc) == STORFS_OK)
        {
                break;
        }
        find_next_open_byte_helper(storfsInst, &currentOpenFile->fileLoc);
    }
    

    //Find the next available open byte
    if(find_update_next_open_byte(storfsInst, newOpenFile.fileLoc) != STORFS_OK)
    {
        return STORFS_ERROR;
    }   

    return STORFS_OK;
}

static storfs_err_t file_delete_helper(storfs_t *storfsInst, storfs_loc_t storfsLoc, storfs_file_header_t storfsInfo)
{
    int32_t delDataItr = 0;
    storfs_loc_t delDataHeaderLoc = storfsLoc;            //Location of the file to be removed
    storfs_file_header_t currHeaderInfo = storfsInfo;     //Information of the file to be removed

    //Determine the number of iterations for deletion of files 
    delDataItr = (storfsInfo.fileSize + storfsInst->pageSize) / storfsInst->pageSize;
    delDataHeaderLoc.byteLoc = 0;

    do
    {
        STORFS_LOGD(TAG, "Deleting File/Fragment At %ld%ld, %ld", (uint32_t)(delDataHeaderLoc.pageLoc >> 32),(uint32_t)(delDataHeaderLoc.pageLoc),  delDataHeaderLoc.byteLoc);

        //Erase the current page
        if(storfsInst->erase(storfsInst, delDataHeaderLoc.pageLoc) != STORFS_OK)
        {
            STORFS_LOGE(TAG, "Erasing page failed in function remove");
            return STORFS_ERROR;
        }

        delDataItr--;
        if(delDataItr > 0)
        {
            //Set the next location to what is in the erased page's header
            delDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);

            //Store the next locations header information
            if(file_header_store_helper(storfsInst, &currHeaderInfo, delDataHeaderLoc, "") != STORFS_OK)
            {
                STORFS_LOGE(TAG, "Could not read from the current header");
                return STORFS_ERROR;
            }
        }
    } while (delDataItr > 0);

    return STORFS_OK;
}

static storfs_err_t directory_delete_helper(storfs_t *storfsInst, storfs_loc_t rmParentLoc, storfs_file_header_t rmParentHeader)
{
    STORFS_LOGI(TAG, "Deleting directory and all of it's containing files");

    //Remove the directory
    if(file_delete_helper(storfsInst, rmParentLoc, rmParentHeader) != STORFS_OK)
    {
        return STORFS_ERROR;
    }

    //If the parent header has a child location, ensure the children get deleted
    if(rmParentHeader.childLocation != 0x00)
        {
            storfs_file_header_t rmChildHeader;
            storfs_loc_t rmChildFileLoc;
            rmChildFileLoc.pageLoc = LOCATION_TO_PAGE(rmParentHeader.childLocation, storfsInst);
            rmChildFileLoc.byteLoc = LOCATION_TO_BYTE(rmParentHeader.childLocation, storfsInst);

            file_header_store_helper(storfsInst, &rmChildHeader, rmChildFileLoc, "Remove");
            while(1)
            {
                //If a directory needs to be deleted, iterate through this
                if(rmChildHeader.childLocation != 0x00)
                {
                    if(directory_delete_helper(storfsInst, rmChildFileLoc, rmChildHeader) != STORFS_OK)
                    {
                        return STORFS_ERROR;
                    }
                    rmChildFileLoc.pageLoc = LOCATION_TO_PAGE(rmChildHeader.childLocation, storfsInst);
                    rmChildFileLoc.byteLoc = LOCATION_TO_BYTE(rmChildHeader.childLocation, storfsInst);
                }
                else
                {
                    if(file_delete_helper(storfsInst, rmChildFileLoc, rmChildHeader) != STORFS_OK)
                    {
                        return STORFS_ERROR;
                    }
                    rmChildFileLoc.pageLoc = LOCATION_TO_PAGE(rmChildHeader.siblingLocation, storfsInst);
                    rmChildFileLoc.byteLoc = LOCATION_TO_BYTE(rmChildHeader.siblingLocation, storfsInst);
                }

                //If no siblings, this will be the last file within the directory
                if(rmChildHeader.siblingLocation == 0x00)
                {
                    break;
                }
                file_header_store_helper(storfsInst, &rmChildHeader, rmChildFileLoc, "Remove");
            }
        }
       
    return STORFS_OK;
}

static storfs_err_t find_prev_file_loc(storfs_t* storfsInst,  storfs_loc_t storfsCurrLoc, storfs_loc_t storfsItrLoc, storfs_loc_t* storfsPrevLoc)
{
    storfs_file_header_t prevFileHeader;
    storfs_loc_t storfsNextChildLoc;
    storfs_loc_t storfsNextSibLoc;
    
    //Store the iterator header and determine if the child or sibling location is equal to the the current location
    if(file_header_store_helper(storfsInst, &prevFileHeader, storfsItrLoc, "Previous File") != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    if( prevFileHeader.childLocation == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc, storfsCurrLoc.pageLoc, storfsInst) ||
        prevFileHeader.siblingLocation == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc, storfsCurrLoc.pageLoc, storfsInst))
    {
        *storfsPrevLoc = storfsItrLoc;
        return STORFS_OK;
    }

    //Determine whether the previous file has a child
    if(prevFileHeader.childLocation != 0x00)
    {
        storfsNextChildLoc.byteLoc = LOCATION_TO_BYTE(prevFileHeader.childLocation, storfsInst);
        storfsNextChildLoc.pageLoc = LOCATION_TO_PAGE(prevFileHeader.childLocation, storfsInst);

        //If the previous file has a child re-iterate through find_prev_file_loc
        if(find_prev_file_loc(storfsInst, storfsCurrLoc, storfsNextChildLoc, storfsPrevLoc) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

        //If the previously found location is the same as the 
        if(BYTEPAGE_TO_LOCATION(storfsPrevLoc->byteLoc, storfsPrevLoc->pageLoc, storfsInst) == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc, storfsCurrLoc.pageLoc, storfsInst))
        {
            return STORFS_OK;
        }
    }

    //Determine whether the previous file has a sibling
    if(prevFileHeader.siblingLocation != 0x00)
    {
        //If the current header has a sibling location, continuously iterate through the siblings until the wanted location is found or not
        while(prevFileHeader.siblingLocation != 0x00)
        {
            storfsNextSibLoc.byteLoc = LOCATION_TO_BYTE(prevFileHeader.siblingLocation, storfsInst);
            storfsNextSibLoc.pageLoc = LOCATION_TO_PAGE(prevFileHeader.siblingLocation, storfsInst);
            if(file_header_store_helper(storfsInst, &prevFileHeader, storfsNextSibLoc, "Previous File") != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            //If either the child location of the sibling location is equivalent to the wanted location, place that
            if( prevFileHeader.childLocation == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc, storfsCurrLoc.pageLoc, storfsInst) ||
                prevFileHeader.siblingLocation == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc, storfsCurrLoc.pageLoc, storfsInst))
            {
                *storfsPrevLoc = storfsNextSibLoc;
                return STORFS_OK;
            }

            //If there is a child location, iterate through it with the prev_file_loc function
            if(prevFileHeader.childLocation != 0x00)
            {
                storfsNextChildLoc.byteLoc = LOCATION_TO_BYTE(prevFileHeader.childLocation, storfsInst);
                storfsNextChildLoc.pageLoc = LOCATION_TO_PAGE(prevFileHeader.childLocation, storfsInst);
                if(find_prev_file_loc(storfsInst, storfsCurrLoc, storfsNextChildLoc, storfsPrevLoc) != STORFS_OK)
                {
                    return STORFS_ERROR;
                }
                if(BYTEPAGE_TO_LOCATION(storfsPrevLoc->byteLoc, storfsPrevLoc->pageLoc, storfsInst) == BYTEPAGE_TO_LOCATION(storfsCurrLoc.byteLoc, storfsCurrLoc.pageLoc, storfsInst))
                {
                    return STORFS_OK;
                }
            }
        }
    }

    return STORFS_OK;
}

storfs_err_t wear_level_act(storfs_t* storfsInst, wear_level_t *wearLevelInfo)
{
    uint8_t relocateBuf[storfsInst->pageSize];
    wear_level_t prevWearLevelInfo;

    //Store the previous header, determine if it was a fragment header or a file/directory/root header
    file_header_store_helper(storfsInst, &prevWearLevelInfo.storfsInfo, wearLevelInfo->storfsPrevLoc, "Previous File");

    //If the previous file information is a file, the root or a directory... else if it is a fragment
    if((prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_FILE || 
        (prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_ROOT || 
        (prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_DIRECTORY)
    {
        //Determine the parent/sibling location of the previous file in order to prepare it for another iteration of wear-level writing
        if(find_prev_file_loc(storfsInst, wearLevelInfo->storfsPrevLoc, storfsInst->cachedInfo.rootLocation[0], &prevWearLevelInfo.storfsPrevLoc) != STORFS_OK)
        {
            LOGE(TAG, "Error determining the previous file's parent/sibling location");
            return STORFS_ERROR;
        }

        //The header info location will be the same as the previous location
        prevWearLevelInfo.storfsInfoLoc = prevWearLevelInfo.storfsPrevLoc;
        
        //Set the previous header length
        prevWearLevelInfo.headerLen = STORFS_HEADER_TOTAL_SIZE;

        //Set the page size that must be re-written
        if(prevWearLevelInfo.storfsInfo.fileSize > storfsInst->pageSize)
        {
            prevWearLevelInfo.sendDataLen  = storfsInst->pageSize;
        }
        else
        {
            prevWearLevelInfo.sendDataLen  = prevWearLevelInfo.storfsInfo.fileSize;
        }

        //Determine if the child location or sibling location must be re-written
        if(prevWearLevelInfo.storfsInfo.childLocation == BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsOrigLoc.byteLoc, wearLevelInfo->storfsOrigLoc.pageLoc, storfsInst) ||
            wearLevelInfo->storfsFlags & STORFS_FILE_PARENT_FLAG)
        {
            STORFS_LOGI(TAG, "Updating previous file child location");
            prevWearLevelInfo.storfsInfo.childLocation = BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc, wearLevelInfo->storfsCurrLoc->pageLoc, storfsInst);
        }
        else if(prevWearLevelInfo.storfsInfo.siblingLocation == BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsOrigLoc.byteLoc, wearLevelInfo->storfsOrigLoc.pageLoc, storfsInst) ||
                wearLevelInfo->storfsFlags & STORFS_FILE_SIBLING_FLAG)
        {
            STORFS_LOGI(TAG, "Updating previous file sibling location");
            prevWearLevelInfo.storfsInfo.siblingLocation = BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc, wearLevelInfo->storfsCurrLoc->pageLoc, storfsInst);
        }
    }
    else
    {
        storfs_file_header_t tempHeader = wearLevelInfo->storfsInfo;

        //If the current head file header's fragment location is equal to the previous files fragment location
        if(tempHeader.fragmentLocation == BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsPrevLoc.byteLoc, wearLevelInfo->storfsPrevLoc.pageLoc, storfsInst))
        {
            //The previous location will be equivalent to the main header's location
            prevWearLevelInfo.storfsPrevLoc = wearLevelInfo->storfsInfoLoc;
        }
        else
        {
            //Iterate through fragment header locations until the previous fragment header is found
            while(tempHeader.fragmentLocation != BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsPrevLoc.byteLoc, wearLevelInfo->storfsPrevLoc.pageLoc, storfsInst))
            {
                prevWearLevelInfo.storfsPrevLoc.pageLoc = LOCATION_TO_PAGE(tempHeader.fragmentLocation, storfsInst);
                prevWearLevelInfo.storfsPrevLoc.byteLoc = LOCATION_TO_BYTE(tempHeader.fragmentLocation, storfsInst);

                //Update the prevWearLevel header information while iterating
                file_header_store_helper(storfsInst, &tempHeader, prevWearLevelInfo.storfsPrevLoc, "Previous Fragment");
            }
        }

        STORFS_LOGI(TAG, "Updating previous file fragment location");

        //Set the previous header length to be a fragment header length
        prevWearLevelInfo.headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;

        //Set the previous header's info location to the original
        prevWearLevelInfo.storfsInfoLoc = wearLevelInfo->storfsInfoLoc;

        //Set the page size that must be re-written, must be a full page if there is a fragment
        prevWearLevelInfo.sendDataLen  = storfsInst->pageSize;

        //Update the fragment location
        prevWearLevelInfo.storfsInfo.fragmentLocation = BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc, wearLevelInfo->storfsCurrLoc->pageLoc, storfsInst);
    }

    //Determine if the previous file is a directory or the root and ensure that the correct flag is applied to the next iteration
    if((prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_DIRECTORY || 
    (prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_ROOT || 
    (((prevWearLevelInfo.storfsInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_FILE) && (prevWearLevelInfo.sendDataLen == STORFS_HEADER_TOTAL_SIZE)))
    {
        prevWearLevelInfo.storfsFlags = STORFS_FILE_HEADER_WRITE;
    }
    else
    {
        prevWearLevelInfo.storfsFlags = STORFS_FILE_WRITE_FLAG;
    }

    STORFS_LOGI(TAG, "Previous file's, previous file location %ld%ld", (uint32_t)(BYTEPAGE_TO_LOCATION(prevWearLevelInfo.storfsPrevLoc.byteLoc, prevWearLevelInfo.storfsPrevLoc.pageLoc, storfsInst) >> 32), (uint32_t)(BYTEPAGE_TO_LOCATION(prevWearLevelInfo.storfsPrevLoc.byteLoc, prevWearLevelInfo.storfsPrevLoc.pageLoc, storfsInst)));
    
    //Convert the header to a buffer, read the previous file, erase it and write the new information to it
    file_info_display_helper(prevWearLevelInfo.storfsInfo);
    info_to_buf(relocateBuf, &prevWearLevelInfo.storfsInfo);
    if(storfsInst->read(storfsInst,wearLevelInfo->storfsPrevLoc.pageLoc, prevWearLevelInfo.headerLen, (relocateBuf + prevWearLevelInfo.headerLen), (storfsInst->pageSize - prevWearLevelInfo.headerLen)) != STORFS_OK)
    {
        return STORFS_READ_FAILED;
    }
    if(storfsInst->erase(storfsInst, wearLevelInfo->storfsPrevLoc.pageLoc) != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    
    //Write to the previous wear struct the previous files information
    prevWearLevelInfo.sendBuf = relocateBuf;
    prevWearLevelInfo.storfsCurrLoc = &wearLevelInfo->storfsPrevLoc;
    prevWearLevelInfo.storfsOrigLoc = wearLevelInfo->storfsPrevLoc;

    //Write the previous file and header
    write_wear_level_helper(storfsInst, &prevWearLevelInfo);

    return STORFS_OK;
}

static storfs_err_t write_wear_level_helper(storfs_t* storfsInst, wear_level_t *wearLevelInfo)
{
    wear_level_state_t state = WRITE_BAD;
    uint8_t itr = 0;

    //Write to the area in memory and then check the crc and determine if that page in memory is worn/not usable
    while(1)
    {
        STORFS_LOGD(TAG, "Writing File At %ld%ld, %ld", (uint32_t)(wearLevelInfo->storfsCurrLoc->pageLoc >> 32),(uint32_t)(wearLevelInfo->storfsCurrLoc->pageLoc), wearLevelInfo->storfsCurrLoc->byteLoc);

        //Retry write if failed to the page a certain amount of times based on user defined value
        for(int i = 0; i < STORFS_WEAR_LEVEL_RETRY_NUM; i++)
        {
            if(i > 0)
            {
                STORFS_LOGW(TAG, "Failed to write to location, re-writting to location");
            }
            
            //If the programming functionality fails return an error
            if(storfsInst->write(storfsInst, wearLevelInfo->storfsCurrLoc->pageLoc, wearLevelInfo->storfsCurrLoc->byteLoc, wearLevelInfo->sendBuf, wearLevelInfo->sendDataLen) != STORFS_OK)
            {
                STORFS_LOGE(TAG, "Writing to memory failed in function fputs");
                return STORFS_WRITE_FAILED;
            }
            if(storfsInst->sync(storfsInst) != STORFS_OK)
            {
                return STORFS_ERROR;
            }
            if(wearLevelInfo->storfsFlags & STORFS_FILE_INIT_HEADER_WRITE || 
                wearLevelInfo->storfsFlags & STORFS_FILE_HEADER_WRITE)
            {
                //If CRC returned correctly, break
                if(crc_header_check(storfsInst, *wearLevelInfo->storfsCurrLoc) == STORFS_OK)
                {
                    if(itr == 0)
                    {
                        state = WRITE_GOOD;
                    }
                    else
                    {
                        state = WRITE_RELOCATE;
                    }
                    break;
                }
            }
            else
            {
                if(crc_file_check(storfsInst, *wearLevelInfo->storfsCurrLoc, (wearLevelInfo->sendDataLen - wearLevelInfo->headerLen)) == STORFS_OK)
                {
                    if(itr == 0)
                    {
                        state = WRITE_GOOD;
                    }
                    else
                    {
                        state = WRITE_RELOCATE;
                    }
                    break;
                }
            } 
            if(storfsInst->erase(storfsInst, wearLevelInfo->storfsCurrLoc->pageLoc) != STORFS_OK)
            {
                LOGE(TAG, "Could not erase page in wear-level function");
                return STORFS_ERROR;
            }           
        }

        //If written successful, continue
        if(state == WRITE_GOOD || state == WRITE_RELOCATE)
        {
            break;
        }

        //If CRC returns incorrectly, find another location to write to
        find_next_open_byte_helper(storfsInst, wearLevelInfo->storfsCurrLoc);

        //If this is a file being written to, it is the first write and the send data length is greater than a page size, the fragment location must be updated as well
        if(wearLevelInfo->storfsFlags & STORFS_FILE_WRITE_FLAG && 
            wearLevelInfo->storfsFlags & STORFS_FILE_WRITE_INIT_FLAG &&
            wearLevelInfo->sendDataLen >= storfsInst->pageSize)
        {
            storfs_loc_t nextFragmentLoc = *wearLevelInfo->storfsCurrLoc;
            storfs_file_header_t currInfo;
            
            find_next_open_byte_helper(storfsInst, &nextFragmentLoc);
            buf_to_info(wearLevelInfo->sendBuf, &currInfo);
            currInfo.fragmentLocation = BYTEPAGE_TO_LOCATION(nextFragmentLoc.byteLoc, nextFragmentLoc.pageLoc, storfsInst);
            info_to_buf(wearLevelInfo->sendBuf, &currInfo);
        }

        itr++;
    }

    //If a file was rewritten to a new location than what was expected, the previous file must be re-written with new location
    //Or if it is the initial write to a header file, the previous file must be updated to the newest position
    if(state == WRITE_RELOCATE || wearLevelInfo->storfsFlags & STORFS_FILE_INIT_HEADER_WRITE)
    {
        if(wearLevelInfo->storfsPrevLoc.pageLoc == storfsInst->cachedInfo.rootLocation[0].pageLoc &&
            wearLevelInfo->storfsPrevLoc.byteLoc == storfsInst->cachedInfo.rootLocation[0].byteLoc)
        {
            storfsInst->cachedInfo.rootHeaderInfo[0].childLocation = BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc, wearLevelInfo->storfsCurrLoc->pageLoc, storfsInst);
            storfsInst->cachedInfo.rootHeaderInfo[1].childLocation = BYTEPAGE_TO_LOCATION(wearLevelInfo->storfsCurrLoc->byteLoc, wearLevelInfo->storfsCurrLoc->pageLoc, storfsInst);
            return STORFS_OK;
        }
        wear_level_act(storfsInst, wearLevelInfo);
    }

    return STORFS_OK;
}

storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName)
{
    storfs_file_header_t firstPartInfo[2];
    uint32_t strLen = 0;

    STORFS_LOGI(TAG, "Mounting File System");

    //If the user defined region for the first root directory byte added to the size of the header is larger than the size of a page, return an error
    if((storfsInst->firstByteLoc + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize)
    {
        STORFS_LOGE(TAG, "The user defined starting byte and header size is larger than the user defined page size");
        return STORFS_ERROR;
    }

    //Store the first partition location into the cache
    storfsInst->cachedInfo.rootLocation[0].pageLoc = storfsInst->firstPageLoc;
    storfsInst->cachedInfo.rootLocation[0].byteLoc = storfsInst->firstByteLoc;

    //The second root header will be a page ahead of the first root header
    storfsInst->cachedInfo.rootLocation[1].byteLoc = 0;
    storfsInst->cachedInfo.rootLocation[1].pageLoc = storfsInst->cachedInfo.rootLocation[0].pageLoc + 1; 
    
    //Store the header written to the storage and verify that the crc is correct TODO
    file_header_store_helper(storfsInst,  &firstPartInfo[0], storfsInst->cachedInfo.rootLocation[0], "Root");
    file_info_display_helper(firstPartInfo[0]);
    file_header_store_helper(storfsInst,  &firstPartInfo[1], storfsInst->cachedInfo.rootLocation[1], "Root");
    file_info_display_helper(firstPartInfo[1]);

    //If file is empty create the root partition within the user defined parameters
    //The system will use two headers for the root
    if(((firstPartInfo[0].fileInfo & STORFS_INFO_REG_BLOCK_SIGN_EMPTY) == 0x60) || ((firstPartInfo[1].fileInfo & STORFS_INFO_REG_BLOCK_SIGN_EMPTY) == 0x60))
    {      
        //Ensure that both of the roots are cleared
        if(storfsInst->erase(storfsInst, storfsInst->cachedInfo.rootLocation[0].pageLoc) != STORFS_OK)
        {
            return STORFS_ERROR;
        }  
        if(storfsInst->erase(storfsInst, storfsInst->cachedInfo.rootLocation[1].pageLoc) != STORFS_OK)
        {
            return STORFS_ERROR;
        }
        //Set next open byte
        storfsInst->cachedInfo.nextOpenByte = ((storfsInst->cachedInfo.rootLocation[1].pageLoc + 1) * storfsInst->pageSize);

        //Get string length
        while(partName[strLen++] != '\0');

        //Error checking
        if(strLen == 0 || (storfsInst->cachedInfo.nextOpenByte >= (storfsInst->pageCount * storfsInst->pageSize)))
        {
            STORFS_LOGE(TAG, "STORfs cannot be mounted");
            return STORFS_ERROR;
        }

        //Store file parameters
        for(int i = 0; i < strLen; i++)
        {
            firstPartInfo[0].fileName[i] = partName[i];
        }
        firstPartInfo[0].fileInfo = STORFS_INFO_REG_BLOCK_SIGN_PART_FULL | STORFS_INFO_REG_FILE_TYPE_ROOT;
        firstPartInfo[0].childLocation = storfsInst->cachedInfo.nextOpenByte;
        firstPartInfo[0].siblingLocation = 0x0;
        firstPartInfo[0].reserved = 0xFFFF;
        firstPartInfo[0].fragmentLocation = storfsInst->cachedInfo.nextOpenByte;
        firstPartInfo[0].fileSize = STORFS_HEADER_TOTAL_SIZE * 2;
        firstPartInfo[0].crc = STORFS_CRC_CALC(storfsInst, firstPartInfo[0].fileName, strLen);
        firstPartInfo[1] = firstPartInfo[0];

        //Write data to first available memory location defined by user
        if(file_header_create_helper(storfsInst, &firstPartInfo[0], storfsInst->cachedInfo.rootLocation[0], "Root") != STORFS_OK)
        {
            STORFS_LOGE(TAG, "The filesystem could not be created at location %ld%ld, %ld", (uint32_t)(storfsInst->cachedInfo.rootLocation[0].pageLoc >> 32), \
                (uint32_t)(storfsInst->cachedInfo.rootLocation[0].pageLoc), storfsInst->cachedInfo.rootLocation[0].byteLoc);
            return STORFS_ERROR; 
        }

        file_header_store_helper(storfsInst,  &firstPartInfo[0], storfsInst->cachedInfo.rootLocation[0], "Root");
        file_info_display_helper(firstPartInfo[0]);

        //Compare the CRC obtained from the file to the computed crc of the filename
        if(crc_compare(storfsInst, firstPartInfo[0], firstPartInfo[0].fileName, strLen) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

        //Write data to second available memory location following the first
        if(file_header_create_helper(storfsInst, &firstPartInfo[1], storfsInst->cachedInfo.rootLocation[1], "Root") != STORFS_OK)
        {
            STORFS_LOGE(TAG, "The filesystem could not be created at location %ld%ld, %ld", (uint32_t)(storfsInst->cachedInfo.rootLocation[1].pageLoc >> 32), \
                (uint32_t)(storfsInst->cachedInfo.rootLocation[1].pageLoc), storfsInst->cachedInfo.rootLocation[1].byteLoc);
            return STORFS_ERROR; 
        }

        file_header_store_helper(storfsInst,  &firstPartInfo[1], storfsInst->cachedInfo.rootLocation[1], "Root");
        file_info_display_helper(firstPartInfo[1]);

        //Compare the CRC obtained from the file to the computed crc of the filename
        if(crc_compare(storfsInst, firstPartInfo[1], firstPartInfo[1].fileName, strLen) != STORFS_OK)
        {
            return STORFS_ERROR;
        }
        

        storfsInst->cachedInfo.rootHeaderInfo[0] = firstPartInfo[0];
        storfsInst->cachedInfo.rootHeaderInfo[1] = firstPartInfo[1];
    }
    else
    {        
        //Get string length
        while(firstPartInfo[0].fileName[strLen++] != '\0');

        //Compare the CRC code to the register code
        if(crc_compare(storfsInst, firstPartInfo[0], firstPartInfo[0].fileName, strLen) != STORFS_OK)
        {
            return STORFS_ERROR;
        }
        
        //Get string length
        strLen = 0;
        while(firstPartInfo[1].fileName[strLen++] != '\0');
        
        //Compare the CRC code to the register code
        if(crc_compare(storfsInst, firstPartInfo[1], firstPartInfo[1].fileName, strLen) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

        //Set next open byte
        storfsInst->cachedInfo.nextOpenByte = firstPartInfo[1].fragmentLocation;
    }
    
    return STORFS_OK;
}

storfs_err_t storfs_mkdir(storfs_t *storfsInst, char *pathToDir)
{   
    STORFS_LOGI(TAG, "Making Directory at %s", pathToDir);

    return file_handling_helper(storfsInst, (storfs_name_t *)pathToDir, DIR_CREATE, NULL);
}

storfs_err_t storfs_touch(storfs_t *storfsInst, char *pathToFile)
{
    STORFS_LOGI(TAG, "Making File at %s", pathToFile);

    return file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_CREATE, NULL);
}

storfs_err_t storfs_fopen(storfs_t *storfsInst, char *pathToFile, const char * mode, STORFS_FILE *stream)
{
    STORFS_LOGI(TAG, "Opening File at %s in %s mode", pathToFile, mode);
    storfs_file_flags_t fileFlags = 0;

    if(file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_OPEN, stream) != STORFS_OK)
    {
        STORFS_LOGE(TAG, "Cannot open or create file");
        goto ERR;
    }

    //Determine the flags to write to the file
    if(strcmp(mode, "w") == 0)
    {
        //Determine if the file is already populated with data, and if it is delete the file associated
        if(stream->fileInfo.fileSize > STORFS_HEADER_TOTAL_SIZE)
        {
            if(fopen_write_flag_helper(storfsInst, pathToFile, stream) != STORFS_OK)
            {
                goto ERR;
            }   
        }

        fileFlags = STORFS_FILE_WRITE_FLAG;
    }
    else if(strcmp(mode, "r") == 0)
    {
        fileFlags = STORFS_FILE_READ_FLAG;
    }
    else if(strcmp(mode, "a") == 0)
    {
        fileFlags = STORFS_FILE_APPEND_FLAG;
    }
    else if(strcmp(mode, "w+") == 0)
    {
        //Determine if the file is already populated with data, and if it is delete the file associated
        if(stream->fileInfo.fileSize > STORFS_HEADER_TOTAL_SIZE)
        {
            if(fopen_write_flag_helper(storfsInst, pathToFile, stream) != STORFS_OK)
            {
                goto ERR;
            }
        }

        fileFlags = STORFS_FILE_WRITE_FLAG | STORFS_FILE_READ_FLAG;
    }
    else if(strcmp(mode, "r+") == 0)
    {
        fileFlags = STORFS_FILE_WRITE_FLAG | STORFS_FILE_READ_FLAG;
    }
    else if(strcmp(mode, "a+") == 0)
    {
        fileFlags = STORFS_FILE_APPEND_FLAG | STORFS_FILE_READ_FLAG;
    }
    else
    {
        //Default file flag to file read
        fileFlags = STORFS_FILE_READ_FLAG;
    }
    
    //Rewind the file back to the original location, unset rewind flag
    storfs_rewind(storfsInst, stream);
    stream->fileFlags &= ~(STORFS_FILE_REWIND_FLAG);

    //Set the current file flags as the file flags for the returned FILE struct
    stream->fileFlags = fileFlags;

    STORFS_LOGD(TAG, "File Location: %ld%ld, %ld \r\n \
    File Flags: %ld", (uint32_t)(stream->fileLoc.pageLoc >> 32), (uint32_t)(stream->fileLoc.pageLoc ), stream->fileLoc.byteLoc, fileFlags);

    return STORFS_OK;

    ERR:
        return STORFS_ERROR;
}

storfs_err_t storfs_fputs(storfs_t *storfsInst, const char *str, const int n, STORFS_FILE *stream)
{
    //Sanity Check
    if(storfsInst == NULL || stream == NULL || str == NULL || n == 0 || stream == NULL)
    {
        STORFS_LOGE(TAG, "Cannot write to file");
        return STORFS_ERROR;
    }

    //Error if next write is larger than the page count
    if(LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) >= storfsInst->pageCount)
    {
        STORFS_LOGE(TAG, "Cannot write any more data to the file system");
    }

    //If the file is opened in read only return an error
    if(stream->fileFlags == STORFS_FILE_READ_FLAG)
    {
        STORFS_LOGE(TAG, "Cannot write to file, in read only mode");
        return STORFS_ERROR;
    }

    STORFS_LOGI(TAG, "Writing to file %s", stream->fileInfo.fileName);

    uint8_t sendBuf[storfsInst->pageSize];                                    //Buffer of data to send to flash device                                      
    uint8_t headerBuf[STORFS_HEADER_TOTAL_SIZE];                              //Buffer used to store the header of each page
    uint32_t headerLen = STORFS_HEADER_TOTAL_SIZE;                            //Length of header to be used depending on fragment header or file header
    int count = n;                                                            //Length of the data to be placed in storage
    int32_t sendDataItr = 0;                                                  //Iterations for the number of pages to be programmed
    int32_t currItr = 0;
    uint32_t sendDataLen;                                                     //The current length of data to be sent to the file system
    wear_level_t wearLevelInfo;                                               //Needed for wear level writing

    storfs_loc_t currDataHeaderLoc = stream->fileLoc;                         //Location of the current data
    storfs_loc_t nextDataHeaderLoc = currDataHeaderLoc;                       //Next location for data to be written to
    storfs_loc_t prevDataHeaderLoc = stream->filePrevLoc;
    
    storfs_file_size_t updatedFileSize;                                       //Updated filesize to be written to the header
    storfs_file_header_t currHeaderInfo;                                      //Current Header's information
    
    int32_t appendHeaderByteLoc = 0;                                          //Location of the data to be appended onto the current buffer

    //Get updated file information
    if(file_header_store_helper(storfsInst, &stream->fileInfo, stream->fileLoc, "Updated") != STORFS_OK)
    {
        return STORFS_ERROR;
    }

    if(stream->fileFlags & STORFS_FILE_APPEND_FLAG && stream->fileInfo.fileSize > STORFS_HEADER_TOTAL_SIZE && !(stream->fileFlags & STORFS_FILE_REWIND_FLAG))
    {
        //Update the file size of the main header
        updatedFileSize = stream->fileInfo.fileSize + n + ((n / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) * STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

        //Store the file header
        currHeaderInfo = stream->fileInfo;

        //Find the current location of the header to be appended on to
        currDataHeaderLoc.byteLoc = 0;
        while(currHeaderInfo.fragmentLocation != 0x00)
        {
            //Set the previous header location to the current
            prevDataHeaderLoc = currDataHeaderLoc;

            //Set the current header location to the fragment location
            currDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);
            file_header_store_helper(storfsInst, &currHeaderInfo, currDataHeaderLoc, "Append");
        }
       
        //If the location of the next available byte is greater than the files original location...
        if(currDataHeaderLoc.pageLoc != stream->fileLoc.pageLoc)
        {
            STORFS_LOGD(TAG, "Appending to file fragment");

            //Append needed to write onto the current buffer length
            appendHeaderByteLoc = (stream->fileInfo.fileSize % storfsInst->pageSize) - STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
            if(appendHeaderByteLoc < 0)
            {
                appendHeaderByteLoc = 0;
            }

            //Update the file size register in the header of the file
            stream->fileInfo.fileSize = updatedFileSize;
            if(storfsInst->read(storfsInst, stream->fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE, (sendBuf + STORFS_HEADER_TOTAL_SIZE), (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK)
            {
                return STORFS_READ_FAILED;
            }

            //Delete the file header so it may be written to
            if(storfsInst->erase(storfsInst, stream->fileLoc.pageLoc) != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            //Write the new file header with the updated information
            info_to_buf(sendBuf, &stream->fileInfo);
            if(storfsInst->write(storfsInst,  stream->fileLoc.pageLoc, 0, sendBuf, storfsInst->pageSize) != STORFS_OK)
            {
                return STORFS_WRITE_FAILED;
            }

            //Read in the current header of the data buffer
            if(storfsInst->read(storfsInst, currDataHeaderLoc.pageLoc, STORFS_FRAGMENT_HEADER_TOTAL_SIZE, (sendBuf + STORFS_FRAGMENT_HEADER_TOTAL_SIZE), (appendHeaderByteLoc - STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) != STORFS_OK)
            {
                return STORFS_READ_FAILED;
            }
            //Delete the page from memory so it may be re-written
            if(storfsInst->erase(storfsInst, currDataHeaderLoc.pageLoc) != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            //Set the header length to fragment header size
            headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
        }
        else
        {
            STORFS_LOGD(TAG, "Appending to file head");

            //Append needed to write onto the current buffer length
            appendHeaderByteLoc = stream->fileInfo.fileSize - STORFS_HEADER_TOTAL_SIZE;
            if(appendHeaderByteLoc < 0)
            {
                appendHeaderByteLoc = 0;
            }

            //Read in the current header of the data buffer
            if(storfsInst->read(storfsInst, stream->fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE, (sendBuf + STORFS_HEADER_TOTAL_SIZE), (appendHeaderByteLoc - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK)
            {
                return STORFS_READ_FAILED;
            }
            //Delete the page from memory so it may be re-written
            if(storfsInst->erase(storfsInst, stream->fileLoc.pageLoc) != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            //Set the current filesize information
            currHeaderInfo.fileSize = updatedFileSize;
        }

        //Adjust the count of data to be written to
        count+=appendHeaderByteLoc;
        
        STORFS_LOGD(TAG, "Append File Location: %ld%ld, %ld", (uint32_t)(currDataHeaderLoc.pageLoc >> 32),(uint32_t)currDataHeaderLoc.pageLoc, appendHeaderByteLoc + headerLen);

        //Determine the number of iterations that must be programmed to the device
        sendDataItr = (count + storfsInst->pageSize) / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE);
        
        //Set the next data header location to this location
        nextDataHeaderLoc = currDataHeaderLoc;

        //Adjust reading file size remainder
        stream->fileRead.fileSizeRem -= appendHeaderByteLoc;
    }
    else
    {
        //Store the current header so it may be updated when initially writting to memory
        //file_header_store_helper(storfsInst, &currHeaderInfo, currDataHeaderLoc, "Write Function");
        currHeaderInfo = stream->fileInfo;

        // Delete the file to be written to
        file_delete_helper(storfsInst, currDataHeaderLoc, currHeaderInfo);

        //Update the file size register
        updatedFileSize = STORFS_HEADER_TOTAL_SIZE + n + ((n / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) * STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

        //Update the file size register in the header of the file
        currHeaderInfo.fileSize = updatedFileSize;

        //Determine the number of iterations that must be programmed to the device
        sendDataItr = 1;
        if((count + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize)
        {
            sendDataItr += ((count - (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) + storfsInst->pageSize) / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE);
        }

        //Reset reading file size remainder and file read pointer
        stream->fileRead.fileSizeRem = 0;
        stream->fileRead.readLocPtr.pageLoc = stream->fileLoc.pageLoc;
        stream->fileRead.readLocPtr.byteLoc = STORFS_HEADER_TOTAL_SIZE;
    }

    do
    {   
        //Determine which type of header to store
        if(currItr > 0)
        {
            headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
            currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_FILE_TYPE_FILE);
            currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_NOT_FRAGMENT_BIT);
        }
        else
        {
            currHeaderInfo.fileInfo |= STORFS_INFO_REG_NOT_FRAGMENT_BIT;
        }
        

        //If the string length is greater than a page size ensure the sent data can maximally be the page size
        if((count + headerLen) > storfsInst->pageSize)
        {
            sendDataLen = storfsInst->pageSize;
            count -= (storfsInst->pageSize - headerLen);

            //Determine where the fragment location will be at
            if((storfsInst->cachedInfo.nextOpenByte < BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc, storfsInst)) && currItr == 0)
            {
                nextDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst);
            }
            else
            {
                find_next_open_byte_helper(storfsInst, &nextDataHeaderLoc);
            }
            
            //Set the file header to full in the current header
            currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_BLOCK_SIGN_EMPTY);
            currHeaderInfo.fileInfo |= STORFS_INFO_REG_BLOCK_SIGN_FULL;

            //Update the fragment register in the previous header with the new fragment location
            currHeaderInfo.fragmentLocation = BYTEPAGE_TO_LOCATION(nextDataHeaderLoc.byteLoc, nextDataHeaderLoc.pageLoc, storfsInst);
            
        }
        else
        {
            sendDataLen = count + headerLen;

            //If the total size is written to the page then set the file info flag as block full of data
            if(sendDataLen == storfsInst->pageSize)
            {
                currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_BLOCK_SIGN_EMPTY);
                currHeaderInfo.fileInfo |= STORFS_INFO_REG_BLOCK_SIGN_FULL;
            }
            else
            {
                currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_BLOCK_SIGN_EMPTY);
                currHeaderInfo.fileInfo |= STORFS_INFO_REG_BLOCK_SIGN_PART_FULL;
            }

            currHeaderInfo.fragmentLocation = 0x00;
        }

        //Convert the current header info into a buffer and store it in the first bytes to be programmed
        //Store the data to be programmed as well in the buffer
        for(int i = headerLen; i < sendDataLen; i++)
        {
            //If there is items to append to the current buffer
            if(currItr == 0 && ((i - headerLen) < appendHeaderByteLoc))
            {
                continue;
            }
            else
            {
                sendBuf[i] = str[i - headerLen - appendHeaderByteLoc];
            }
        }

        //Calculate CRC
        currHeaderInfo.crc = STORFS_CRC_CALC(storfsInst, (uint8_t*)(sendBuf + headerLen), (sendDataLen - headerLen));

        //Place Header into buffer
        info_to_buf(headerBuf, &currHeaderInfo);
        for(int i = 0; i < headerLen; i++)
        {
            sendBuf[i] = headerBuf[i];
        }

        //Wear level handling for information
        wearLevelInfo.headerLen = headerLen;
        wearLevelInfo.sendBuf = sendBuf;
        wearLevelInfo.sendDataLen = sendDataLen;
        wearLevelInfo.storfsCurrLoc = &currDataHeaderLoc;
        wearLevelInfo.storfsOrigLoc = currDataHeaderLoc;
        wearLevelInfo.storfsPrevLoc = prevDataHeaderLoc;
        wearLevelInfo.storfsInfo = stream->fileInfo;
        wearLevelInfo.storfsInfoLoc = stream->fileLoc;
        wearLevelInfo.storfsFlags = STORFS_FILE_WRITE_FLAG | STORFS_FILE_WRITE_INIT_FLAG;
        if(write_wear_level_helper(storfsInst, &wearLevelInfo) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

        //Decrement the number of iterations left
        --sendDataItr;

        //Increment the buffer's location to send data
        str += (sendDataLen - headerLen - appendHeaderByteLoc) * sizeof(uint8_t);

        //Set current header location equal to the next, and previous to current
        if(wearLevelInfo.storfsCurrLoc->pageLoc >= nextDataHeaderLoc.pageLoc)
        {
            //Update nextOpenByte to what is available
            find_next_open_byte_helper(storfsInst, &currDataHeaderLoc);
            storfsInst->cachedInfo.nextOpenByte = BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc, storfsInst);

            //If the current header location is equivalent to the stream's original file location and there was an error writing, update the stream's file location
            if(currDataHeaderLoc.pageLoc == stream->fileLoc.pageLoc)
            {
                stream->fileLoc = *wearLevelInfo.storfsCurrLoc;
            }
        }
        else
        {
            currDataHeaderLoc = nextDataHeaderLoc;
        }
        prevDataHeaderLoc = *wearLevelInfo.storfsCurrLoc;

        //Increment current iteration number
        currItr++;

        //Increment read file size remainder
        stream->fileRead.fileSizeRem += (sendDataLen - headerLen);
        STORFS_LOGD(TAG, "Read File Size Remainder %ld", stream->fileRead.fileSizeRem);

        //Set the append header byte location to 0
        if(appendHeaderByteLoc > 0)
        {
            appendHeaderByteLoc = 0;
        }
    } while (sendDataItr > 0);

    //Store the updated header into the file information
    if(file_header_store_helper(storfsInst,  &stream->fileInfo, stream->fileLoc, "Updated FILE") != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    
    //Find and update the next open byte available if the next open byte is currently larger than the file's location
    if(storfsInst->cachedInfo.nextOpenByte <= BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc, storfsInst))
    {
        currDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) - 1;
        find_update_next_open_byte(storfsInst, currDataHeaderLoc);
    } 
    else
    {
        //Update the root with current values possibly overwritten when using wear-levelling
        update_root(storfsInst);
    }

    if(stream->fileFlags & STORFS_FILE_REWIND_FLAG)
    {
        LOGD(TAG, "Rewound file has been written");
        stream->fileFlags &= ~(STORFS_FILE_REWIND_FLAG);
    }

    file_info_display_helper(stream->fileInfo);

    return STORFS_OK;
}

storfs_err_t storfs_fgets(storfs_t *storfsInst, char *str, int n, STORFS_FILE *stream)
{
    if(storfsInst == NULL || stream == NULL || stream->fileFlags == STORFS_FILE_DELETED_FLAG)
    {
        STORFS_LOGE(TAG, "Cannot read from file, it does not exist");
        return STORFS_ERROR;
    }
    if(stream->fileFlags == STORFS_FILE_WRITE_FLAG || stream->fileFlags == STORFS_FILE_APPEND_FLAG)
    {
        STORFS_LOGE(TAG, "Cannot read file, in incorrect mode");
        return STORFS_ERROR;
    }

    STORFS_LOGI(TAG, "Reading from file %s", stream->fileInfo.fileName);

    int32_t recvDataItr = 0;                                    //Iterations to read from file
    uint32_t recvDataLen;                                       //Current length to read from file
    storfs_file_header_t currHeaderInfo;                        //Info of the current in the file
    uint32_t headerLen = STORFS_HEADER_TOTAL_SIZE;              //Fragment or file header length
    int count = n;                                              //Storage for total number of bytes to be read from the file
    storfs_loc_t recvDataHeaderLoc;                             //The location of the current file header in memory
    
    recvDataHeaderLoc.pageLoc = stream->fileRead.readLocPtr.pageLoc;
    recvDataHeaderLoc.byteLoc = 0;

    file_header_store_helper(storfsInst, &currHeaderInfo, recvDataHeaderLoc, "fgets");
    
    //Determine the number of iterations needed to read from the file
    if(count < stream->fileRead.fileSizeRem)
    {
        if(count > storfsInst->pageSize)
        {
            recvDataItr = (count + STORFS_HEADER_TOTAL_SIZE + ((count / storfsInst->pageSize) * STORFS_FRAGMENT_HEADER_TOTAL_SIZE) + storfsInst->pageSize) / storfsInst->pageSize;
        }
        else
        {
            recvDataItr = (count + STORFS_HEADER_TOTAL_SIZE + storfsInst->pageSize) / storfsInst->pageSize;
        }
    }
    else
    {
        recvDataItr = (stream->fileRead.fileSizeRem + storfsInst->pageSize) / storfsInst->pageSize;
        count = stream->fileRead.fileSizeRem;
    }
    
    STORFS_LOGW(TAG, "File Size count %d", count);

    do
    {
        STORFS_LOGD(TAG, "Reading File At %ld%ld, %ld", (uint32_t)(stream->fileRead.readLocPtr.pageLoc >> 32),(uint32_t)(stream->fileRead.readLocPtr.pageLoc),  stream->fileRead.readLocPtr.byteLoc);

        //If the receive string buffer length is greater than a page size ensure the received data will maximally be the page size   
        if((count + headerLen) > (storfsInst->pageSize - stream->fileRead.readLocPtr.byteLoc))
        {
            recvDataLen = (storfsInst->pageSize - stream->fileRead.readLocPtr.byteLoc);
            count -= recvDataLen;
        }
        else
        {
            recvDataLen = count;
        }

        //Read in the data and store each page size in the buffer
        if(storfsInst->read(storfsInst, stream->fileRead.readLocPtr.pageLoc, stream->fileRead.readLocPtr.byteLoc, (uint8_t *)str, recvDataLen) != STORFS_OK)
        {
            STORFS_LOGE(TAG, "Reading from memory failed in function fgets");
            return STORFS_READ_FAILED;
        }
        if(storfsInst->sync(storfsInst) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

        recvDataItr--;

        //Decrement read file size remainder
        stream->fileRead.fileSizeRem -= recvDataLen;

        //If there are fragments...
        if(recvDataItr > 0)
        {
            //Find the next fragments location
            stream->fileRead.readLocPtr.pageLoc = LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);
            stream->fileRead.readLocPtr.byteLoc = 0;
            file_header_store_helper(storfsInst, &currHeaderInfo, stream->fileRead.readLocPtr, "");

            //The next fragment's data will be after it's header
            headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
            stream->fileRead.readLocPtr.byteLoc = headerLen;

            //Increment the buffer's location to store data
            str += recvDataLen * sizeof(uint8_t);
        }
    } while (recvDataItr > 0);

    //Ensure the file size remainder does not go below zero
    if(stream->fileRead.fileSizeRem < 0)
    {
        stream->fileRead.fileSizeRem = 0;
    }  
    STORFS_LOGD(TAG, "Read File Size Remainder %ld", stream->fileRead.fileSizeRem);

    //Set read pointer byte location
    stream->fileRead.readLocPtr.byteLoc += recvDataLen;

    return STORFS_OK;
}

storfs_err_t storfs_rm(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *stream)
{
    //Error Checking
    if(storfsInst == NULL || pathToFile == NULL)
    {
        return STORFS_ERROR;
    }

    STORFS_FILE rmStream;
    storfs_file_header_t storfsPreviousHeader;
    STORFS_LOGI(TAG, "Removing file at %s", pathToFile);

    //Open the file again in order to find the current parent/sibling/children
    if(file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_OPEN, &rmStream) != STORFS_OK)
    {
        return STORFS_ERROR;
    }

    //If the item to delete is a file or directory
    if((rmStream.fileInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_FILE)
    {
        if(stream != NULL)
        {
            //Set the stream flag to Deleted so it may not be used again until opened/created
            stream->fileFlags = STORFS_FILE_DELETED_FLAG;
        }

        if(file_delete_helper(storfsInst, rmStream.fileLoc, rmStream.fileInfo) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

    }
    else
    {
       if(directory_delete_helper(storfsInst, rmStream.fileLoc, rmStream.fileInfo) != STORFS_OK)
       {
           return STORFS_ERROR;
       }
    }
    
    
    //Store the previous header and manipulate it
    file_header_store_helper(storfsInst, &storfsPreviousHeader, rmStream.filePrevLoc, "Previous");

    //Update the child or sibling directory of the previous file location
    if(rmStream.filePrevLoc.pageLoc == storfsInst->firstPageLoc)
    {
        storfsInst->cachedInfo.rootHeaderInfo[0].childLocation = rmStream.fileInfo.siblingLocation;
        storfsInst->cachedInfo.rootHeaderInfo[1].childLocation = rmStream.fileInfo.siblingLocation;
    }
    else if(rmStream.filePrevFlags == STORFS_FILE_PARENT_FLAG)
    {
        storfsPreviousHeader.childLocation = rmStream.fileInfo.siblingLocation;
        //Remove the header from storage so it may be re-written
        if(storfsInst->erase(storfsInst, rmStream.filePrevLoc.pageLoc) != STORFS_OK)
        {
            return STORFS_ERROR;
        }
        if(file_header_create_helper(storfsInst, &storfsPreviousHeader, rmStream.filePrevLoc, "") != STORFS_OK)
        {
            return STORFS_ERROR;
        }
    }
    else
    {
        //If the current file being deleted has a sibling, update the previous register's sibling with the current registers sibling
        storfsPreviousHeader.siblingLocation = rmStream.fileInfo.siblingLocation;

         //If the previous file is a directory simply update the header, if not read in the data of the original file page and update the page with a new header
        if((storfsPreviousHeader.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_DIRECTORY)
        {
            //Remove the header from storage so it may be re-written
            if(storfsInst->erase(storfsInst, rmStream.filePrevLoc.pageLoc) != STORFS_OK)
            {
                return STORFS_ERROR;
            }
            if(file_header_create_helper(storfsInst, &storfsPreviousHeader, rmStream.filePrevLoc, "") != STORFS_OK)
            {
                return STORFS_ERROR;
            }
        }
        else
        {
            uint8_t siblingBuf[storfsInst->pageSize];
            uint8_t updatedHeader[STORFS_HEADER_TOTAL_SIZE];

            STORFS_LOGD(TAG, "Updating Previous File Sibling Location at the file's initial location at %ld%ld, %d", (uint32_t)(rmStream.filePrevLoc.pageLoc >> 32), (uint32_t)(rmStream.filePrevLoc.pageLoc), 0);

            if(storfsInst->read(storfsInst, rmStream.fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE, siblingBuf, (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK)
            {
                return STORFS_READ_FAILED;
            }

            //Remove the header from storage so it may be re-written
            if(storfsInst->erase(storfsInst, rmStream.filePrevLoc.pageLoc) != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            info_to_buf(updatedHeader, &storfsPreviousHeader);
            for(int i = 0; i < storfsInst->pageSize; i++)
            {
                if(i < STORFS_HEADER_TOTAL_SIZE)
                {
                    siblingBuf[i] = updatedHeader[i];
                }
                else
                {
                    siblingBuf[i] = siblingBuf[i - STORFS_HEADER_TOTAL_SIZE];
                }
            }
            if(storfsInst->write(storfsInst, rmStream.filePrevLoc.pageLoc, 0, siblingBuf, storfsInst->pageSize) != STORFS_OK)
            {
                return STORFS_WRITE_FAILED;
            }
        }
    }

    //Update the next open byte to the file that was deleted if the next open byte is currently larger than the files location
    if(storfsInst->cachedInfo.nextOpenByte >= BYTEPAGE_TO_LOCATION(rmStream.fileLoc.byteLoc, rmStream.fileLoc.pageLoc, storfsInst))
    {
        update_root_next_open_byte(storfsInst, BYTEPAGE_TO_LOCATION(rmStream.fileLoc.byteLoc, rmStream.fileLoc.pageLoc, storfsInst));
    }
    
    return STORFS_OK;
}

storfs_err_t storfs_rewind(storfs_t *storfsInst, STORFS_FILE *stream)
{
    if(storfsInst == NULL || stream == NULL || stream->fileFlags == STORFS_FILE_DELETED_FLAG)
    {
        STORFS_LOGE(TAG, "Error in opening the current file stream");
        return STORFS_ERROR;
    }

    LOGI(TAG, "Rewinding file %s to original location", stream->fileInfo.fileName);

    //Set read pointer location
    stream->fileRead.readLocPtr.pageLoc = stream->fileLoc.pageLoc;
    stream->fileRead.readLocPtr.byteLoc = STORFS_HEADER_TOTAL_SIZE;
    //Set read file size remainder
    stream->fileRead.fileSizeRem = stream->fileInfo.fileSize - STORFS_HEADER_TOTAL_SIZE - (stream->fileInfo.fileSize / storfsInst->pageSize * STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

    LOGD(TAG, "File size remainder %ld", stream->fileRead.fileSizeRem);

    //Set rewind flag
    stream->fileFlags |= STORFS_FILE_REWIND_FLAG;

    return STORFS_OK;
}

storfs_err_t storfs_display_header(storfs_t *storfsInst, storfs_loc_t loc)
{
    storfs_file_header_t header;
    if(file_header_store_helper(storfsInst, &header, loc, "Test") != STORFS_OK)
    {
        return STORFS_ERROR;
    }
    file_info_display_helper(header);

    return STORFS_OK;
}
