#include "storfs.h"
#include "core.h"

storfs_err_t storfs_fgets(storfs_t *storfsInst, char *str, int n, STORFS_FILE *stream)
{
    if(storfsInst == NULL || stream == NULL || stream->fileFlags == STORFS_FILE_DELETED_FLAG)
    {
        STORFS_LOGE(TAG, "Cannot read from file, it does not exist");
        return STORFS_ERR_NULL_POINTER;
    }

    if(stream->fileFlags == STORFS_FILE_WRITE_FLAG || stream->fileFlags == STORFS_FILE_APPEND_FLAG)
    {
        STORFS_LOGE(TAG, "Cannot read file, in incorrect mode");
        return STORFS_ERR_INVALID_MODE;
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
    
    //If the count is zero, the file has been completely read, warn the user
    if(count == 0)
    {        
        STORFS_LOGW(TAG, "File has been completely read");
        return STORFS_OK;
    }

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
            return STORFS_ERR_READ_FAILED;
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

