#include "storfs.h"
#include "core.h"
#include <string.h>

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
