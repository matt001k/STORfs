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
#define STORFS_FILE_WRITE_FLAG                  0x01
#define STORFS_FILE_READ_FLAG                   0x02
#define STORFS_FILE_APPEND_FLAG                 0x04
#define STORFS_FILE_PARENT_FLAG                 0x0A
#define STORFS_FILE_SIBLING_FLAG                0x0B
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
static void update_root_next_open_byte(storfs_t *storfsInst, storfs_size_t fileLocation);
static storfs_err_t find_next_open_byte_helper (storfs_t *storfsInst, storfs_loc_t *storfsLoc);

/** @brief Function to handle opening/creating new files, most important function of STORfs */
static storfs_err_t file_handling_helper(storfs_t *storfsInst, storfs_name_t *pathToDir, file_action_t actionFlag, void *buff);

/** @brief File open helper function for w or w+ modes */
static storfs_err_t fopen_write_flag_helper(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *currentOpenFile);

/** @brief Helper functions to delete directories and files */
static storfs_err_t file_delete_helper(storfs_t *storfsInst, storfs_loc_t storfsLoc, storfs_file_header_t storfsInfo);
static storfs_err_t directory_delete_helper(storfs_t *storfsInst, storfs_loc_t rmParentLoc, storfs_file_header_t rmParentHeader);

static storfs_err_t crc_compare(storfs_t *storfsInst, storfs_file_header_t storfsInfo, const uint8_t *buf, uint32_t bufLen)
{
    if(storfsInfo.crc == (STORFS_CRC_CALC(storfsInst, buf, bufLen)))
    {
        LOGD(TAG, "CRC Code Correct");
        return STORFS_OK;
    }

    LOGE(TAG, "CRC Code Returned Incorrectly");
    return STORFS_CRC_ERR;
}

static storfs_err_t crc_header_check(storfs_t *storfsInst, storfs_loc_t storfsLoc)
{
    storfs_file_header_t storfsInfo;
    uint32_t strLen = 0;

    file_header_store_helper(storfsInst, &storfsInfo, storfsLoc, "CRC Check");

    while(storfsInfo.fileName[strLen++] != '\0');

    return crc_compare(storfsInst, storfsInfo, (uint8_t *)storfsInfo.fileName, strLen);
}

static storfs_err_t crc_file_check(storfs_t *storfsInst, storfs_loc_t storfsLoc, uint32_t len)
{
    storfs_file_header_t storfsInfo;
    uint32_t headerLen = STORFS_HEADER_TOTAL_SIZE;
    uint8_t buf[len];

    file_header_store_helper(storfsInst, &storfsInfo, storfsLoc, "CRC Check");
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
        LOGI(TAG, "\t fileInfo %x \r\n \
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
#endif

static storfs_err_t file_header_create_helper(storfs_t *storfsInst, storfs_file_header_t *storfsInfo, storfs_loc_t storfsLoc, const char *string)
{
    storfs_err_t status = STORFS_OK;
    uint8_t headerBuf[STORFS_HEADER_TOTAL_SIZE];

    //If header to create is overflowing the user defined page size or there is no space left in the storage device return an error
    if(((storfsLoc.byteLoc + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize) )//TODO add overflow fault
    {
        status = STORFS_WRITE_FAILED;
        goto FUNEND;
    }

    //Turn the header information into a buffer to write to memory
    LOGD(TAG, "Writing %s Header at %ld%ld, %ld", string, (uint32_t)(storfsLoc.pageLoc >> 32), \
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

    LOGD(TAG, "Storing %s Header at %ld%ld, %ld", string, (uint32_t)(storfsLoc.pageLoc >> 32), \
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

static void update_root_next_open_byte(storfs_t *storfsInst, storfs_size_t fileLocation)
{
    //Update the cached information with the next open byte
    storfsInst->cachedInfo.nextOpenByte = fileLocation;

    //Update the next open byte info in the cached rootInfo
    storfsInst->cachedInfo.rootHeaderInfo[0].fragmentLocation = fileLocation;
    storfsInst->cachedInfo.rootHeaderInfo[1].fragmentLocation = fileLocation;

    //Update both root registers
    file_header_create_helper(storfsInst, &storfsInst->cachedInfo.rootHeaderInfo[0], storfsInst->cachedInfo.rootLocation[0], "Root Header 1");
    file_header_create_helper(storfsInst, &storfsInst->cachedInfo.rootHeaderInfo[1], storfsInst->cachedInfo.rootLocation[1], "Root Header 1");

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
    LOGD(TAG, "Finding and updating next open byte");
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
    storfs_file_header_t currentDirInfo;                                    //Information for the current directory searched through
    storfs_loc_t currentLocation = storfsInst->cachedInfo.rootLocation[0];  //Location of the current directory to obtain information
    STORFS_FILE previousFile;                                               //Information and location of the previous file
    uint8_t fileSepCnt = 0;                                                 //File separator count to ensure files do no have children
    path_flag_t pathFlag = PATH_LEFT;

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
                    LOGE(TAG, "Directory name cannot have an extension");
                    return STORFS_ERROR; 
                }
                fileSepCnt++;
            }
            currentFileName[currStr++] = pathToDir[strLen++];
        }
        currentFileName[currStr] = '\0';
        LOGD(TAG, "File name %s", currentFileName);

        if(pathToDir[strLen] == '\0')
        {
            pathFlag = PATH_LAST;
        }

        do
        {
            //Store the current file header
            if(file_header_store_helper(storfsInst,  &currentDirInfo, currentLocation, "Directory") != STORFS_OK)
            {
                return STORFS_ERROR;
            }

            //Does the current file header name equal to the name in the path?
            if(strcmp((const char *)currentDirInfo.fileName, (const char *)currentFileName) == 0)
            {
                //If the filename is matched it is a parent directory, update the previous file information with the current
                LOGD(TAG, "File name matched: %s", currentFileName);

                 if(pathFlag == PATH_LAST)
                {
                    break;
                }

                //If there is no child location update the child's location to the next open byte
                if(currentDirInfo.childLocation == 0x0)
                {
                    currentDirInfo.childLocation = storfsInst->cachedInfo.nextOpenByte;
                }

                previousFile.fileLoc = currentLocation;
                previousFile.fileInfo = currentDirInfo;
                previousFile.filePrevFlags = STORFS_FILE_PARENT_FLAG;

                //Continue to search the child's location if it is not the last line throughout the path
                previousFile.filePrevLoc = currentLocation;
                currentLocation.pageLoc = LOCATION_TO_PAGE(currentDirInfo.childLocation, storfsInst);
                currentLocation.byteLoc = LOCATION_TO_BYTE(currentDirInfo.childLocation, storfsInst);
            }
            else if (currentDirInfo.siblingLocation != 0xFFFFFFFFFFFFFFFF)
            {
                //If there is no sibling location update the sibling's location to the next open byte
                if(currentDirInfo.siblingLocation == 0x0)
                {
                    currentDirInfo.siblingLocation = storfsInst->cachedInfo.nextOpenByte;
                }

                //If the filename is not matched and the sibling location exists, it is a sibling directory
                //Update the previous file information with the current
                LOGD(TAG, "Name not matched, searching siblings");
                previousFile.fileLoc = currentLocation;
                previousFile.filePrevFlags = STORFS_FILE_SIBLING_FLAG;
                previousFile.fileInfo = currentDirInfo;

                //Continue to search the siblings's location
                previousFile.filePrevLoc = currentLocation;
                currentLocation.pageLoc = LOCATION_TO_PAGE(currentDirInfo.siblingLocation, storfsInst);
                currentLocation.byteLoc = LOCATION_TO_BYTE(currentDirInfo.siblingLocation, storfsInst);
            }
            else
            {
                LOGD(TAG, "Name not matched, and no siblings, creating file/directory at next open location");

                //File's cannot be children of other files
                if(fileSepCnt > 1)
                {
                    LOGE(TAG, "File/directory cannot be a child of another file");
                    return STORFS_ERROR;
                }

                //Set information for the header of the current file directory
                for(int i = 0; i <= currStr; i++)
                {
                    currentDirInfo.fileName[i] = currentFileName[i];
                }
                currentDirInfo.reserved = 0xFFFF;
                currentDirInfo.fileSize = STORFS_HEADER_TOTAL_SIZE;

                //Set the current directory fragment, sibling and child location registers to zero
                //These locations will never be zero as long as the file system exists, it is a safe value to set these
                currentDirInfo.siblingLocation = 0;
                currentDirInfo.childLocation = 0;
                currentDirInfo.fragmentLocation = 0;

                //Compute CRC of the filename given
                currentDirInfo.crc = STORFS_CRC_CALC(storfsInst, currentDirInfo.fileName, (currStr+1));

                //If the file size will be the size of the page size, ensure the file info sets the information for the file to full
                if(actionFlag == DIR_CREATE)
                {
                    currentDirInfo.fileInfo = STORFS_INFO_REG_FILE_TYPE_DIRECTORY | STORFS_INFO_REG_BLOCK_SIGN_FULL;
                }
                else
                {
                    currentDirInfo.fileInfo = STORFS_INFO_REG_FILE_TYPE_FILE | STORFS_INFO_REG_BLOCK_SIGN_PART_FULL;
                }

                //Determine whether the previous file's child location or sibling location register has to be updated
                if(previousFile.filePrevFlags == STORFS_FILE_PARENT_FLAG)
                {
                    previousFile.fileInfo.childLocation = BYTEPAGE_TO_LOCATION(currentLocation.byteLoc, currentLocation.pageLoc, storfsInst);
                    if(file_header_create_helper(storfsInst, &previousFile.fileInfo, previousFile.fileLoc, "") != STORFS_OK)
                    {
                        return STORFS_ERROR;
                    }
                }
                else
                {
                    previousFile.fileInfo.siblingLocation = BYTEPAGE_TO_LOCATION(currentLocation.byteLoc, currentLocation.pageLoc, storfsInst);
                    //If the previous file is a directory simply update the header, if not read in the data of the original file page and update the page with a new header
                    if((previousFile.fileInfo.fileInfo & STORFS_INFO_REG_FILE_TYPE_FILE) == STORFS_INFO_REG_FILE_TYPE_DIRECTORY)
                    {
                        if(file_header_create_helper(storfsInst, &previousFile.fileInfo, previousFile.fileLoc, "") != STORFS_OK)
                        {
                            return STORFS_ERROR;
                        }
                    }
                    else
                    {
                        //Update the header at the previous location by re-writting the whole page
                        uint8_t siblingBuf[storfsInst->pageSize];
                        uint8_t updatedHeader[STORFS_HEADER_TOTAL_SIZE];

                        LOGD(TAG, "Updating Previous File Sibling Location at the file's initial location at %ld%ld, %d", (uint32_t)(previousFile.fileLoc.pageLoc >> 32), (uint32_t)(previousFile.fileLoc.pageLoc), 0);

                        //Store the data from the page
                        if(storfsInst->read(storfsInst, previousFile.fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE, siblingBuf, (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK)
                        {
                            return STORFS_READ_FAILED;
                        }

                        //Turn the header and stored data into a programmable buffer
                        info_to_buf(updatedHeader, &previousFile.fileInfo);
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
                        if(storfsInst->write(storfsInst, previousFile.fileLoc.pageLoc, previousFile.fileLoc.byteLoc, siblingBuf, storfsInst->pageSize) != STORFS_OK)
                        {
                            return STORFS_WRITE_FAILED;
                        }
                    }
                }

                while(1)
                {
                    //Create the header for the new file/directory
                    if(file_header_create_helper(storfsInst, &currentDirInfo, currentLocation, "") != STORFS_OK)
                    {
                        return STORFS_ERROR;
                    }
                    if(crc_header_check(storfsInst, currentLocation) == STORFS_OK)
                    {
                        break;
                    }
                    find_next_open_byte_helper(storfsInst, &currentLocation);
                }

                //Display the newly created file information
                file_info_display_helper(currentDirInfo);

                //Determine the next open byte for the cache
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
        previousFile.fileInfo = currentDirInfo;
        *(STORFS_FILE *)buff = previousFile;
    }

    return STORFS_OK;
}

static storfs_err_t fopen_write_flag_helper(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *currentOpenFile)
{
    STORFS_FILE newOpenFile = *currentOpenFile;

    //Remove the file since it exists 
    if(file_delete_helper(storfsInst, currentOpenFile->fileLoc, currentOpenFile->fileInfo) != STORFS_OK)
    {
        LOGE(TAG, "Cannot delete the old file");
        return STORFS_ERROR;
    }
    
    //Recreate file header
    if(file_header_create_helper(storfsInst, &currentOpenFile->fileInfo, currentOpenFile->fileLoc, "Deleting old file and opening new") != STORFS_OK)
    {
        LOGE(TAG, "Cannot create the old file");
        return STORFS_ERROR;
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
        LOGD(TAG, "Deleting File/Fragment At %ld%ld, %ld", (uint32_t)(delDataHeaderLoc.pageLoc >> 32),(uint32_t)(delDataHeaderLoc.pageLoc),  delDataHeaderLoc.byteLoc);

        //Erase the current page
        if(storfsInst->erase(storfsInst, delDataHeaderLoc.pageLoc) != STORFS_OK)
        {
            LOGE(TAG, "Erasing page failed in function remove");
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
                LOGE(TAG, "Could not read from the current header");
                return STORFS_ERROR;
            }
        }
    } while (delDataItr > 0);

    return STORFS_OK;
}

static storfs_err_t directory_delete_helper(storfs_t *storfsInst, storfs_loc_t rmParentLoc, storfs_file_header_t rmParentHeader)
{
    LOGI(TAG, "Deleting directory and all of it's containing files");

    if(file_delete_helper(storfsInst, rmParentLoc, rmParentHeader) != STORFS_OK)
    {
        return STORFS_ERROR;
    }
     if(rmParentHeader.childLocation != 0x00)
        {
            storfs_file_header_t rmChildHeader;
            storfs_loc_t rmChildFileLoc;
            rmChildFileLoc.pageLoc = LOCATION_TO_PAGE(rmParentHeader.childLocation, storfsInst);
            rmChildFileLoc.byteLoc = LOCATION_TO_BYTE(rmParentHeader.childLocation, storfsInst);

            file_header_store_helper(storfsInst, &rmChildHeader, rmChildFileLoc, "Remove");
            while(1)
            {
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
                if(rmChildHeader.siblingLocation == 0x00)
                {
                    break;
                }
                file_header_store_helper(storfsInst, &rmChildHeader, rmChildFileLoc, "Remove");
                file_info_display_helper(rmChildHeader);
            }
        }
       
    return STORFS_OK;
}

storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName)
{
    storfs_file_header_t firstPartInfo[2];
    uint32_t strLen = 0;

    LOGI(TAG, "Mounting File System");

    //If the user defined region for the first root directory byte added to the size of the header is larger than the size of a page, return an error
    if((storfsInst->firstByteLoc + STORFS_HEADER_TOTAL_SIZE) > storfsInst->pageSize)
    {
        LOGE(TAG, "The user defined starting byte and header size is larger than the user defined page size");
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
        //Set next open byte
        storfsInst->cachedInfo.nextOpenByte = ((storfsInst->cachedInfo.rootLocation[1].pageLoc + 1) * storfsInst->pageSize);

        //Get string length
        while(partName[strLen++] != '\0');

        //Error checking
        if(strLen == 0 || (storfsInst->cachedInfo.nextOpenByte >= (storfsInst->pageCount * storfsInst->pageSize)))
        {
            LOGE(TAG, "STORfs cannot be mounted");
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
        firstPartInfo[0].crc = STORFS_CRC_CALC(storfs, firstPartInfo[0].fileName, strLen);
        firstPartInfo[1] = firstPartInfo[0];

        //Write data to first available memory location defined by user
        if(file_header_create_helper(storfsInst, &firstPartInfo[0], storfsInst->cachedInfo.rootLocation[0], "Root") != STORFS_OK)
        {
            LOGE(TAG, "The filesystem could not be created at location %ld%ld, %ld", (uint32_t)(storfsInst->cachedInfo.rootLocation[0].pageLoc >> 32), \
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
            LOGE(TAG, "The filesystem could not be created at location %ld%ld, %ld", (uint32_t)(storfsInst->cachedInfo.rootLocation[1].pageLoc >> 32), \
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
    LOGI(TAG, "Making Directory at %s", pathToDir);

    return file_handling_helper(storfsInst, (storfs_name_t *)pathToDir, DIR_CREATE, NULL);
}

storfs_err_t storfs_touch(storfs_t *storfsInst, char *pathToFile)
{
    LOGI(TAG, "Making File at %s", pathToFile);

    return file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_CREATE, NULL);
}

storfs_err_t storfs_fopen(storfs_t *storfsInst, char *pathToFile, const char * mode, STORFS_FILE *stream)
{
    LOGI(TAG, "Opening File at %s in %s mode", pathToFile, mode);
    storfs_file_flags_t fileFlags = 0;

    if(file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_OPEN, stream) != STORFS_OK)
    {
        LOGE(TAG, "Cannot open or create file");
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
    
    //Set the current file flags as the file flags for the returned FILE struct
    stream->fileFlags = fileFlags;

    LOGD(TAG, "File Location: %ld%ld, %ld \r\n \
    File Flags: %d", (uint32_t)(stream->fileLoc.pageLoc >> 32), (uint32_t)(stream->fileLoc.pageLoc ), stream->fileLoc.byteLoc, fileFlags);

    return STORFS_OK;

    ERR:
        return STORFS_ERROR;
}

storfs_err_t storfs_fputs(storfs_t *storfsInst, const char *str, const int n, STORFS_FILE *stream)
{
    //Sanity Check
    if(storfsInst == NULL || stream == NULL || str == NULL || n == 0 || stream == NULL)
    {
        LOGE(TAG, "Cannot write to file");
        return STORFS_ERROR;
    }

    //If the file is opened in read only return an error
    if(stream->fileFlags == STORFS_FILE_READ_FLAG)
    {
        LOGE(TAG, "Cannot write to file, in read only mode");
        return STORFS_ERROR;
    }

    LOGI(TAG, "Writing to file %s", stream->fileInfo.fileName);

    uint8_t sendBuf[storfsInst->pageSize];                                    //Buffer of data to send to flash device                                      
    uint8_t headerBuf[STORFS_HEADER_TOTAL_SIZE];                              //Buffer used to store the header of each page
    uint32_t headerLen = STORFS_HEADER_TOTAL_SIZE;                            //Length of header to be used depending on fragment header or file header
    int count = n;                                                            //Length of the data to be placed in storage
    int32_t sendDataItr = 0;                                                  //Iterations for the number of pages to be programmed
    int32_t currItr = 0;
    uint32_t sendDataLen;                                                     //The current length of data to be sent to the file system

    storfs_loc_t currDataHeaderLoc = stream->fileLoc;                         //Location of the current data
    storfs_loc_t nextDataHeaderLoc = currDataHeaderLoc;                       //Next location for data to be written t
    
    storfs_file_size_t updatedFileSize;                                       //Updated filesize to be written to the header
    storfs_file_header_t currHeaderInfo;                                      //Current Header's information
    
    int32_t appendHeaderByteLoc = 0;                                          //Location of the data to be appended onto the current buffer
    uint8_t currDataBuf[storfsInst->pageSize];                                //Buffer to send data in the case of an append 

    //Get update file information
    if(file_header_store_helper(storfsInst, &stream->fileInfo, stream->fileLoc, "Updated") != STORFS_OK)
    {
        return STORFS_ERROR;
    }

    if(stream->fileFlags & STORFS_FILE_APPEND_FLAG && stream->fileInfo.fileSize > STORFS_HEADER_TOTAL_SIZE)
    {
        //Update the file size of the main header
        updatedFileSize = stream->fileInfo.fileSize + n + ((n / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE)) * STORFS_FRAGMENT_HEADER_TOTAL_SIZE);

        //Store the current header
        currHeaderInfo = stream->fileInfo;

        //Update the file size register in the header of the file
        currHeaderInfo.fileSize = updatedFileSize;
        if(storfsInst->read(storfsInst, currDataHeaderLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE, currDataBuf, (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK)
        {
            return STORFS_READ_FAILED;
        }
        info_to_buf(sendBuf, &currHeaderInfo);
        for(int i = STORFS_HEADER_TOTAL_SIZE; i < storfsInst->pageSize; i++)
        {
            sendBuf[i] = currDataBuf[i - STORFS_HEADER_TOTAL_SIZE];
        }
        if(storfsInst->write(storfsInst, currDataHeaderLoc.pageLoc, 0, sendBuf, storfsInst->pageSize) != STORFS_OK)
        {
            return STORFS_WRITE_FAILED;
        }

        //Find the current location of the header to be appended on to
        currDataHeaderLoc.byteLoc = 0;
        while(currHeaderInfo.fragmentLocation != 0x00)
        {
            currDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);
            file_header_store_helper(storfsInst, &currHeaderInfo, currDataHeaderLoc, "Append");
        }

        //Append needed to write onto the current buffer length
        appendHeaderByteLoc = (stream->fileInfo.fileSize % storfsInst->pageSize) - STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
        if(appendHeaderByteLoc < 0)
        {
            appendHeaderByteLoc = 0;
        }
        count+=appendHeaderByteLoc;

        //Read in the current header of the data buffer
        if(storfsInst->read(storfsInst, currDataHeaderLoc.pageLoc, STORFS_FRAGMENT_HEADER_TOTAL_SIZE, currDataBuf, appendHeaderByteLoc) != STORFS_OK)
        {
            return STORFS_READ_FAILED;
        }
        
        LOGD(TAG, "File Location: %ld%ld, %ld", (uint32_t)(currDataHeaderLoc.pageLoc >> 32),(uint32_t)currDataHeaderLoc.pageLoc, appendHeaderByteLoc);

        //Determine the number of iterations that must be programmed to the device
        headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
        sendDataItr = (count + storfsInst->pageSize) / (storfsInst->pageSize - STORFS_FRAGMENT_HEADER_TOTAL_SIZE);
    }
    else
    {
        //Store the current header so it may be updated when initially writting to memory
        file_header_store_helper(storfsInst, &currHeaderInfo, currDataHeaderLoc, "Write Function");

        //If file write flag and if there is more than 1 page length of data, delete the file
        if(currHeaderInfo.fileSize > storfsInst->pageSize)
        {
            file_delete_helper(storfsInst, currDataHeaderLoc, currHeaderInfo);
        }

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
    }

    do
    {   
        LOGD(TAG, "Writing File At %ld%ld, %ld", (uint32_t)(currDataHeaderLoc.pageLoc >> 32),(uint32_t)(currDataHeaderLoc.pageLoc),  currDataHeaderLoc.byteLoc);

        //Determine which type of header to store
        if(currItr > 0)
        {
            headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
            currHeaderInfo.fileInfo &= ~(STORFS_INFO_REG_FILE_TYPE_FILE);
        }

        //If the string length is greater than a page size ensure the sent data can maximally be the page size
        if((count + headerLen) > storfsInst->pageSize)
        {
            sendDataLen = storfsInst->pageSize;
            count -= (storfsInst->pageSize - headerLen);

            //Determine where the fragment location will be at
            if(storfsInst->cachedInfo.nextOpenByte < BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc, storfsInst))
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
            currHeaderInfo.fileInfo &= 0x7F;

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
                sendBuf[i] = currDataBuf[i - headerLen];
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

        //Write to the area in memory and then check the crc and determine if that page in memory is worn/not usable
        while(1)
        {
            //If the programming functionality fails return an error
            if(storfsInst->write(storfsInst, currDataHeaderLoc.pageLoc, currDataHeaderLoc.byteLoc, (uint8_t *)sendBuf, sendDataLen) != STORFS_OK)
            {
                LOGE(TAG, "Writing to memory failed in function fputs");
                return STORFS_WRITE_FAILED;
            }
            if(storfsInst->sync(storfsInst) != STORFS_OK)
            {
                return STORFS_ERROR;
            }
            if(crc_file_check(storfsInst, currDataHeaderLoc, (sendDataLen - headerLen)) == STORFS_OK)
            {
                break;
            }
            find_next_open_byte_helper(storfsInst, &currDataHeaderLoc);
        }

        //Decrement the number of iterations left
        --sendDataItr;

        //Increment the buffer's location to send data
        str += sendDataLen - headerLen - appendHeaderByteLoc;

        //Set current header location equal to the next
        currDataHeaderLoc = nextDataHeaderLoc;

        //Increment current iteration number
        currItr++;

        //Set the append header byte location to 0
        if(appendHeaderByteLoc > 0)
        {
            appendHeaderByteLoc = 0;
        }
    } while (sendDataItr > 0);


    //Store the updated header into the file information
    file_header_store_helper(storfsInst,  &stream->fileInfo, stream->fileLoc, "Updated FILE");
    
    //Find and update the next open byte available if the next open byte is currently larger than the file's location
    if(storfsInst->cachedInfo.nextOpenByte <= BYTEPAGE_TO_LOCATION(currDataHeaderLoc.byteLoc, currDataHeaderLoc.pageLoc, storfsInst))
    {
        currDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(storfsInst->cachedInfo.nextOpenByte, storfsInst) - 1;
        find_update_next_open_byte(storfsInst, currDataHeaderLoc);
    } 

    file_info_display_helper(stream->fileInfo);

    return STORFS_OK;
}

storfs_err_t storfs_fgets(storfs_t *storfsInst, char *str, int n, STORFS_FILE *stream)
{
    if(stream == NULL || stream->fileFlags == STORFS_FILE_DELETED_FLAG)
    {
        LOGE(TAG, "Cannot read from file, it does not exist");
        return STORFS_ERROR;
    }
    if(stream->fileFlags == STORFS_FILE_WRITE_FLAG || stream->fileFlags == STORFS_FILE_APPEND_FLAG)
    {
        LOGE(TAG, "Cannot read file, in incorrect mode");
        return STORFS_ERROR;
    }

    LOGI(TAG, "Reading from file %s", stream->fileInfo.fileName);

    int32_t recvDataItr = 0;                                    //Iterations to read from file
    uint32_t recvDataLen;                                       //Current length to read from file
    storfs_file_header_t currHeaderInfo = stream->fileInfo;     //Info of the current in the file
    uint32_t headerLen = STORFS_HEADER_TOTAL_SIZE;              //Fragment or file header length
    int count = n;                                              //Storage for total number of bytes to be read from the file
    storfs_loc_t recvDataHeaderLoc = stream->fileLoc;           //the location of the current file header in memory
    
    recvDataHeaderLoc.byteLoc = stream->fileLoc.byteLoc + headerLen;
    recvDataItr = (stream->fileInfo.fileSize + storfsInst->pageSize) / storfsInst->pageSize;

    do
    {
        LOGD(TAG, "Reading File At %ld%ld, %ld", (uint32_t)(recvDataHeaderLoc.pageLoc >> 32),(uint32_t)(recvDataHeaderLoc.pageLoc),  recvDataHeaderLoc.byteLoc);

        //If the receive string buffer length is greater than a page size ensure the received data will maximally be the page size   
        if((count + headerLen) > storfsInst->pageSize)
        {
            recvDataLen = (storfsInst->pageSize - headerLen);
            count -= (storfsInst->pageSize - headerLen);
        }
        else
        {
            recvDataLen = count;
        }

        //Read in the data and store each page size in the buffer
        if(storfsInst->read(storfsInst, recvDataHeaderLoc.pageLoc, recvDataHeaderLoc.byteLoc, (uint8_t *)str, recvDataLen) != STORFS_OK)
        {
            LOGE(TAG, "Reading from memory failed in function fgets");
            return STORFS_READ_FAILED;
        }
        if(storfsInst->sync(storfsInst) != STORFS_OK)
        {
            return STORFS_ERROR;
        }

        recvDataItr--;

        //If there are fragments...
        if(recvDataItr > 0)
        {
            //Find the next fragments location
            recvDataHeaderLoc.pageLoc = LOCATION_TO_PAGE(currHeaderInfo.fragmentLocation, storfsInst);
            recvDataHeaderLoc.byteLoc = 0;
            file_header_store_helper(storfsInst, &currHeaderInfo, recvDataHeaderLoc, "");

            //The next fragment's data will be after it's header
            headerLen = STORFS_FRAGMENT_HEADER_TOTAL_SIZE;
            recvDataHeaderLoc.byteLoc = headerLen;

            //Increment the buffer's location to store data
            str += recvDataLen;
            
        }
    } while (recvDataItr > 0);
    
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
    LOGI(TAG, "Removing file at %s", pathToFile);

    //Open the file again in order to find the current parent/sibling/children
    if(file_handling_helper(storfsInst, (storfs_name_t *)pathToFile, FILE_OPEN, &rmStream) != STORFS_OK)
    {
        return STORFS_ERROR;
    }


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
    storfs_file_header_t storfsPreviousHeader;
    file_header_store_helper(storfsInst, &storfsPreviousHeader, rmStream.filePrevLoc, "Previous");

    //Update the child or sibling directory of the previous file location
    if(rmStream.filePrevFlags == STORFS_FILE_PARENT_FLAG)
    {
        storfsPreviousHeader.childLocation = rmStream.fileInfo.siblingLocation;
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
            if(file_header_create_helper(storfsInst, &storfsPreviousHeader, rmStream.filePrevLoc, "") != STORFS_OK)
            {
                return STORFS_ERROR;
            }
        }
        else
        {
            uint8_t siblingBuf[storfsInst->pageSize];
            uint8_t updatedHeader[STORFS_HEADER_TOTAL_SIZE];

            LOGD(TAG, "Updating Previous File Sibling Location at the file's initial location at %ld%ld, %d", (uint32_t)(rmStream.filePrevLoc.pageLoc >> 32), (uint32_t)(rmStream.filePrevLoc.pageLoc), 0);

            if(storfsInst->read(storfsInst, rmStream.fileLoc.pageLoc, STORFS_HEADER_TOTAL_SIZE, siblingBuf, (storfsInst->pageSize - STORFS_HEADER_TOTAL_SIZE)) != STORFS_OK)
            {
                return STORFS_READ_FAILED;
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
