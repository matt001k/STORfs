#include "storfs.h"

/*** Define user uart output for logging here ***/
#ifndef LOGI
    #define LOGI(TAG, fmt, ...)
#endif

/*** Define user wants for file system ***/
#define MAX_PAGES           1028
#define FIRST_PAGE_LOCATION 0
#define FIRST_BYTE_LOCATION 0
#define MEMORY_INSTANCE     NULL
#define PAGE_SIZE           512

static const char *TAG = "Main";

storfs_err_t storfs_read(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
    /*** Define user read function for the file system ***/
}

storfs_err_t storfs_write(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
    /*** Define user write function for the file system ***/
}

storfs_err_t storfs_erase(const struct storfs *storfsInst, storfs_page_t page)
{
    /*** Define user erase function for the file system ***/
}

storfs_err_t storfs_sync(const struct storfs *storfsInst)
{
    /*** Define user sync function for the file system ***/
}

int main(void) {  
    /*** Begin user configuration of needed drivers here 
     * 
     * 
     * 
     * End user configuration of needed drivers here ***/

    storfs_t fs = {
        .read = storfs_read,
        .write = storfs_write,
        .erase = storfs_erase,
        .sync = storfs_sync,
        .memInst = MEMORY_INSTANCE,
        .firstByteLoc = FIRST_BYTE_LOCATION,
        .firstPageLoc = FIRST_PAGE_LOCATION,
        .pageSize = PAGE_SIZE,
        .pageCount = MAX_PAGES,
    };

    /*** Single page write and read test (ensure data is deleted from page to write to) ***/
        uint8_t loadBuffer[4*PAGE_SIZE];
    uint8_t recvBuffer[PAGE_SIZE];
    int charDelim = 33;   
    for(int i = 0; i < (4*PAGE_SIZE); i++)
    {
        loadBuffer[i] = charDelim + i;
        if(charDelim + i > 126)
        {
        charDelim -= (126 - 33);
        }
    }
    //Write to page
    fs.write(&fs, FIRST_PAGE_LOCATION, 0, loadBuffer, PAGE_SIZE);
    fs.sync(&fs);
    fs.read(&fs, FIRST_PAGE_LOCATION, 0, recvBuffer, PAGE_SIZE);
    fs.sync(&fs);

    for(int i = 0; i < PAGE_SIZE; i++)
    {
        if(loadBuffer[i] != recvBuffer[i])
        {
            LOGI(TAG, "The write/read function to a single block has failed");
            break;
        }
    }
    LOGI(TAG, "The write/read function to a single block has completed successfully");

    /*** Multiple page write and read test (ensure data is deleted from page to write to) ***/
    uint8_t err = 0;
    fs.write(&fs, FIRST_PAGE_LOCATION, 0, loadBuffer, PAGE_SIZE);
    fs.sync(&fs);
    fs.write(&fs, FIRST_PAGE_LOCATION + 1, 0, (loadBuffer + PAGE_SIZE), PAGE_SIZE);
    fs.sync(&fs);
    fs.write(&fs, FIRST_PAGE_LOCATION + 2, 0, (loadBuffer + (2 * PAGE_SIZE)), PAGE_SIZE);
    fs.sync(&fs);
    fs.write(&fs, FIRST_PAGE_LOCATION + 3, 0, (loadBuffer + (3 * PAGE_SIZE)), PAGE_SIZE);
    fs.sync(&fs);
    fs.read(&fs, FIRST_PAGE_LOCATION, 0, recvBuffer, PAGE_SIZE);
    fs.sync(&fs);
    for(int i = 0; i < PAGE_SIZE; i++)
    {
        if(loadBuffer[i] != recvBuffer[i])
        {
          err++;
          LOGI(TAG, "The write/read function to multiple blocks (block 1) has failed");
          break;
        }
    }
    fs.read(&fs, FIRST_PAGE_LOCATION + 1, 0, recvBuffer, PAGE_SIZE);
    fs.sync(&fs);
    for(int i = 0; i < PAGE_SIZE; i++)
    {
        if(loadBuffer[i + PAGE_SIZE] != recvBuffer[i])
        {
          err++;
          LOGI(TAG, "The write/read function to mutliple blocks (block 2) has failed");
          break;
        }
    }
    fs.read(&fs, FIRST_PAGE_LOCATION + 2, 0, recvBuffer, PAGE_SIZE);
    fs.sync(&fs);
    for(int i = 0; i < PAGE_SIZE; i++)
    {
        if(loadBuffer[i + (2* PAGE_SIZE)] != recvBuffer[i])
        {
          err++;
          LOGI(TAG, "The write/read function to mutliple blocks (block 3) has failed");
          break;
        }
    }
    fs.read(&fs, FIRST_PAGE_LOCATION + 3, 0, recvBuffer, PAGE_SIZE);
    fs.sync(&fs);
    for(int i = 0; i < PAGE_SIZE; i++)
    {
        if(loadBuffer[i + (3* PAGE_SIZE)] != recvBuffer[i])
        {
          err++;
          LOGI(TAG, "The write/read function to mutliple blocks (block 4) has failed");
          break;
        }
    }
    if(err > 0)
    {
      return 0;
    }
    LOGI(TAG, "The write/read function to multiple blocks has completed successfully");

    /*** Test Erasing a single block ***/
    fs.erase(&fs, FIRST_PAGE_LOCATION);
    fs.sync(&fs);
    fs.read(&fs, FIRST_PAGE_LOCATION, 0, recvBuffer, PAGE_SIZE);
    fs.sync(&fs);
    for(int i = 0; i < PAGE_SIZE; i++)
    {
        if(recvBuffer[i] != 0xFF)
        {
          err++;
          LOGI(TAG, "Erasing a single block has failed");
        }
    }
    if(err > 0)
    {
      return 0;
    }
    LOGI(TAG, "Erasing a single block has completed successfully");

    /*** Test Erasing Multiple blocks ***/
    for(int i = 0; i < 4; i++)
    {
      fs.erase(&fs, (FIRST_PAGE_LOCATION + i));
      fs.sync(&fs);
      fs.read(&fs, (FIRST_PAGE_LOCATION + i), 0, recvBuffer, PAGE_SIZE);
      fs.sync(&fs);
      for(int i = 0; i < PAGE_SIZE; i++)
      {
          if(recvBuffer[i] != 0xFF)
          {
            err++;
            LOGI(TAG, "Erasing multiple blocks (block %d) has failed", i);
          }
      }
    }
    if(err > 0)
    {
      return 0;
    }
    LOGI(TAG, "Erasing a multiple blocks has completed successfully");
}