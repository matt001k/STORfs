#include "storfs.h"

#include <stddef.h>
#include <stdio.h>

#define MEMORYSIMSIZE  33550336

#define PAGESIZE 512
uint8_t memorySim[MEMORYSIMSIZE];

//#define HIGH_VOL_WRITE_TEST

static void display_cache(storfs_t fs)
{
  printf("Cached Data: \n \
  root location 1: %d, %d \n \
  root location 2: %d, %d \n \
  next open byte: %d \n", fs.cachedInfo.rootLocation[0].pageLoc, fs.cachedInfo.rootLocation[0].byteLoc, \
  fs.cachedInfo.rootLocation[1].pageLoc, fs.cachedInfo.rootLocation[1].byteLoc, fs.cachedInfo.nextOpenByte);
}


storfs_err_t storfs_read(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
  if((byte + size) > PAGESIZE)
  {
    return STORFS_ERROR;
  }

  for(int i = 0; i < size; i++)
  {
    buffer[i] = memorySim[(PAGESIZE * page) + i + byte];
  }

  return STORFS_OK;
}

storfs_err_t storfs_write(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
  if((byte + size) > PAGESIZE)
  {
    return STORFS_ERROR;
  }
  for(int i = 0; i < size; i++)
  {
     memorySim[(PAGESIZE * page) + i + byte] = buffer[i];
  }
  return STORFS_OK;
}

storfs_err_t storfs_erase(const struct storfs *storfsInst, storfs_page_t page)
{
  for(int i = 0; i < PAGESIZE; i++)
  {
    memorySim[(PAGESIZE * page) + i] = 0xFF;
  }
return STORFS_OK;
}

storfs_err_t storfs_sync(const struct storfs *storfsInst)
{
return STORFS_OK;
}


int main(void) {  
    
    for (int i = 0; i < MEMORYSIMSIZE; i++)
    {
        memorySim[i] = 0xFF;
    }
    

    storfs_t fs = {
      .read = storfs_read,
      .write = storfs_write,
      .erase = storfs_erase,
      .sync = storfs_sync,
      .memInst = NULL,
      .firstPageLoc = 20,
      .firstByteLoc = 0,
      .pageSize = PAGESIZE,
      .pageCount = 8191,
    };

    printf("%d", STORFS_HEADER_TOTAL_SIZE);

    STORFS_FILE file1;

    //Test mounting root directory c
    storfs_mount(&fs, "C:");

    //After C is created, it should now store items in the cache
    storfs_mount(&fs, "");

    //Test making a directory out of a file
    storfs_mkdir(&fs, "C:/Hello.txt");
    display_cache(fs);

    //Test making directories
    storfs_mkdir(&fs, "C:/HelloDere");
    storfs_mkdir(&fs, "C:/HelloDere/xyz");
    display_cache(fs);
    
    //Test opening and creating files
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "w+", &file1);
    storfs_touch(&fs, "C:/HelloDere/hello.txt");
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "w+", &file1);

    storfs_loc_t location;
    location.byteLoc = 435;
    location.pageLoc = 20;

    storfs_display_header(&fs, location);

    location.byteLoc = 0;

    storfs_mount(&fs, "");


    display_cache(fs);
    storfs_touch(&fs, "C:/YAS.exe");
    storfs_touch(&fs, "C:/DAS.exe");
    printf("Previous File Location %ld%ld, %ld \n", (uint32_t)(file1.filePrevLoc.pageLoc >> 32), (uint32_t)(file1.filePrevLoc.pageLoc), file1.filePrevLoc.byteLoc);

    for(int i = 21; i < 30; i++)
    {
      location.pageLoc = i;
      storfs_display_header(&fs, location);
    }
    


    //Testing writing to a file
    storfs_fputs(&fs, "Hello How are You", 18, &file1);
    display_cache(fs);

    //Test reading from a file
    char buffer[4096];
    storfs_fgets(&fs, buffer, 100, &file1);
    printf("File Read: %s \n", buffer);
    char loadBuffer[1024];
    
    //Test writing and reading large data to a file
    int charDelim = 33;   
    for(int i = 0; i < 1024; i++)
    {
      loadBuffer[i] = charDelim + i;
      if(charDelim + i > 126)
      {
        charDelim -= (126 - 33);
      }
    }
    storfs_fputs(&fs, loadBuffer, 1023, &file1);
    printf("File Write: %s \n", loadBuffer);
    display_cache(fs);
    storfs_fgets(&fs, buffer, 1024, &file1);
    buffer[1023] = '\0';
    printf("File Read: %s \n", buffer);
    int i = 0;
    int buffCount = 0;
    while(buffer[i++] != '\0')
    {
      buffCount++;
    }
    printf("Buff count: %d \n", buffCount);
    display_cache(fs);

    //Test creating files under a file(not supported)
    storfs_touch(&fs, "C:/HelloDere/hello.txt/Jello.txt");

    //Appending to a file
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "a+", &file1);
    storfs_fputs(&fs, "Hello How are You", 17, &file1);
    storfs_fgets(&fs, buffer, 1050, &file1);
    printf("File Read: %s \n", buffer);
    loadBuffer[1023] = '\0';
    storfs_fputs(&fs, loadBuffer, 1024, &file1);
    storfs_fgets(&fs, buffer, 1050 + 1024, &file1);
    printf("File Read: %s \n", buffer);
    display_cache(fs);

    //Test removing a file
    storfs_rm(&fs, "C:/HelloDere/hello.txt", &file1);   
    storfs_fgets(&fs, buffer, 1024, &file1);
    fs.read(&fs, 4, 0, (uint8_t *)buffer, fs.pageSize);
    buffer[fs.pageSize - 1] = fs.pageSize;
    printf("File Read: %s \n", buffer);
    display_cache(fs);

    //Multiple files opened for testing
    STORFS_FILE file2, file3, file4;
    storfs_fopen(&fs, "C:/HelloDere/hello1.txt", "r+", &file2);
    storfs_fopen(&fs, "C:/HelloDere/hello2.txt", "r+", &file3);
    storfs_fopen(&fs, "C:/HelloDere/hello3.txt", "r+", &file4);
    display_cache(fs);

    storfs_fputs(&fs, loadBuffer, 1024, &file2);
    storfs_fputs(&fs, loadBuffer, 512, &file3);
    display_cache(fs);

#ifdef HIGH_VOL_WRITE_TEST
    //High volume write
    uint8_t highVolBuf[100000];
    uint8_t highVolBufRead[100000];
    charDelim = 33;
    for(int i = 0; i < 100000; i++)
    {
      loadBuffer[i] = charDelim + i;
      if(charDelim + i > 126)
      {
        charDelim -= (126 - 33);
      }
    }
    storfs_fputs(&fs, (const char*)highVolBuf, 100000, &file4);
    display_cache(fs);
    storfs_fgets(&fs, (char*)highVolBufRead, 100000, &file4);
    highVolBufRead[99999] = '\0';
    printf("File Read: %s \n", highVolBufRead);

    storfs_fputs(&fs, (char*)highVolBuf, 100000, &file2);
    display_cache(fs);
#endif
    //Test Deleting a siblings of another file
    storfs_rm(&fs, "C:/HelloDere/hello2.txt", &file3);
    display_cache(fs);
    storfs_rm(&fs, "C:/HelloDere/hello.txt", &file1);
    storfs_rm(&fs, "C:/HelloDere/hello1.txt", &file2);
    display_cache(fs);

    //Test to ensure that the next open byte will remain equivalent when writing to a file further down the storage system
    storfs_fputs(&fs, loadBuffer, 1024, &file4);
    display_cache(fs);
    storfs_mkdir(&fs, "C:/Testing12");
    display_cache(fs);
    printf("Next Open Byte %d", fs.cachedInfo.nextOpenByte);

    storfs_mount(&fs, "");

    //Test deleting a directory
    storfs_rm(&fs, "C:/HelloDere", NULL);

    for(int i = 21; i < 30; i++)
    {
      location.pageLoc = i;
      storfs_display_header(&fs, location);
    }

    //Test creating a directory with many files and then deleting it
    storfs_mkdir(&fs, "C:/Testing");
    storfs_fopen(&fs, "C:/Testing/12.txt", "w+", &file1);
    storfs_fopen(&fs, "C:/Testing/123.txt", "a+", &file2);
    storfs_fopen(&fs, "C:/Testing/1234.txt", "a+", &file3);
    storfs_fopen(&fs, "C:/Testing/12345.txt", "w+", &file4);
    storfs_fputs(&fs, loadBuffer, 256, &file1);
    storfs_fputs(&fs, loadBuffer, 1024, &file2);
    storfs_fputs(&fs, loadBuffer, 100, &file3);
    storfs_fputs(&fs, loadBuffer, 512, &file4);
    storfs_mkdir(&fs, "C:/Testing/TEST");
    storfs_fopen(&fs, "C:/Testing/TEST/12.txt", "r+", &file1);
    storfs_fopen(&fs, "C:/Testing/TEST/123.txt", "a+", &file2);
    storfs_fopen(&fs, "C:/Testing/TEST/1234.txt", "a+", &file3);
    storfs_fopen(&fs, "C:/Testing/TEST/12345.txt", "w+", &file4);
    storfs_fputs(&fs, loadBuffer, 256, &file1);
    storfs_fputs(&fs, loadBuffer, 1024, &file2);
    storfs_fputs(&fs, loadBuffer, 100, &file3);
    storfs_fputs(&fs, loadBuffer, 512, &file4);
    storfs_mkdir(&fs, "C:/Testing/TEST/Pest");
    storfs_fopen(&fs, "C:/Testing/TEST/Pest/12.txt", "r+", &file1);
    storfs_fopen(&fs, "C:/Testing/TEST/Pest/123.txt", "a+", &file2);
    storfs_fputs(&fs, loadBuffer, 256, &file1);
    storfs_fputs(&fs, loadBuffer, 1024, &file2);
    storfs_rm(&fs, "C:/Testing", NULL);

    for(int i = 21; i < 35; i++)
    {
      location.pageLoc = i;
      storfs_display_header(&fs, location);
    }

    //Test Truncation of a file
    storfs_mkdir(&fs, "C:/Testing");
    storfs_fopen(&fs, "C:/Testing/12.txt", "w+", &file1);
    storfs_fputs(&fs, loadBuffer, 762, &file1);
    storfs_fputs(&fs, loadBuffer, 1024, &file1);
    storfs_fopen(&fs, "C:/Testing/12.txt", "w+", &file1);
    
  return 1;
}
