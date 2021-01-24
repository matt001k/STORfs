#include "storfs.h"

/*** Define user uart output for logging here ***/
#ifndef LOGI
    #define LOGI(TAG, fmt, ...) \ 
        //LOGI(TAG, fmt, ##VA_ARGS)
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

static void display_cache(storfs_t fs)
{
  LOGI(TAG, "Cached Data: \r\n \
  root location 1: %ld%ld, %ld \r\n \
  root location 2: %ld%ld, %ld \r\n \
  next open byte: %ld%ld", (uint32_t)(fs.cachedInfo.rootLocation[0].pageLoc >> 32), (uint32_t)(fs.cachedInfo.rootLocation[0].pageLoc), fs.cachedInfo.rootLocation[0].byteLoc, \
  (uint32_t)(fs.cachedInfo.rootLocation[1].pageLoc >> 32), (uint32_t)(fs.cachedInfo.rootLocation[1].pageLoc), fs.cachedInfo.rootLocation[1].byteLoc, (uint32_t)(fs.cachedInfo.nextOpenByte >> 32), (uint32_t)fs.cachedInfo.nextOpenByte);
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

    for(int i = 20; i < 55; i++)
    {
        fs.erase(&fs, i);
    }

    storfs_loc_t location;
    location.byteLoc = 0;

    STORFS_FILE file1,file2, file3, file4;

    char buffer[4096];
    char loadBuffer[1024];


    //Test mounting root directory c
    storfs_mount(&fs, "C:");
    //After C is created, it should now store items in the cache
    storfs_mount(&fs, "");
    //Test making a directory out of a file
    storfs_mkdir(&fs, "C:/Hello.txt");
    display_cache(fs);

    //Test making directories
    storfs_mkdir(&fs, "C:/HelloDere");
    display_cache(fs);
    storfs_mkdir(&fs, "C:/HelloDere/xyz");
    display_cache(fs);
    //Test opening and creating files
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "w+", &file1);
    storfs_touch(&fs, "C:/HelloDere/hello.txt");
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "w+", &file1);

    //Re-Mount to test the functionality
    storfs_mount(&fs, "");

    //Creating more files...
    display_cache(fs);
    storfs_touch(&fs, "C:/YAS.exe");
    storfs_touch(&fs, "C:/DAS.exe");

    //Testing writing to a file
    storfs_fputs(&fs, "Hello How are You", 18, &file1);
    display_cache(fs);

    //Test reading from a file
    storfs_fgets(&fs, buffer, 100, &file1);
    LOGI(TAG,"File Read: %s \n", buffer);
        
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
    storfs_fputs(&fs, loadBuffer, 1024, &file1);
    LOGI(TAG, "Write Buffer %s", loadBuffer);

    display_cache(fs);
    storfs_fgets(&fs, buffer, 1024, &file1);
    buffer[1023] = '\0';
    LOGI(TAG, "Read Buffer %s", buffer);
    int i = 0;
    int buffCount = 0;
    while(buffer[i++] != '\0')
    {
        buffCount++;
    }
    LOGI(TAG,"Buff count: %d \n", buffCount);
    display_cache(fs);

    //Test creating files under a file(not supported)
    storfs_touch(&fs, "C:/HelloDere/hello.txt/Jello.txt");
    location.pageLoc = 24;
    storfs_display_header(&fs, location);

    //Test Appending to a file
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "a+", &file1);
    storfs_fputs(&fs, "Hello How are You", 17, &file1);
    storfs_fgets(&fs, buffer, 1024 + 17, &file1);
    buffer[1024 + 17] = '\0';
    LOGI(TAG, "Append Buffer %s", buffer);

    //Test Long Append
    storfs_fputs(&fs, loadBuffer, 1024, &file1);
    storfs_fgets(&fs, buffer, 1050 + 1024, &file1);
    buffer[1024+17+1024] = '\0';
    LOGI(TAG,"File Read: %s \n", buffer);

    //Test removing a file
    storfs_rm(&fs, "C:/HelloDere/hello.txt", &file1);   
    storfs_fgets(&fs, buffer, 1024, &file1);
    fs.read(&fs, 4, 0, (uint8_t *)buffer, fs.pageSize);
    buffer[fs.pageSize - 1] = fs.pageSize;
    printf("File Read: %s \n", buffer);
    display_cache(fs);

    //Open old file and truncate it
    storfs_fopen(&fs, "C:/HelloDere/hello.txt", "w+", &file1);
    display_cache(fs);

    //Multiple files opened for testing
    storfs_fopen(&fs, "C:/HelloDere/hello1.txt", "r+", &file2);
    storfs_fopen(&fs, "C:/HelloDere/hello2.txt", "r+", &file3);
    storfs_fopen(&fs, "C:/HelloDere/hello3.txt", "r+", &file4);
    display_cache(fs);

    storfs_fputs(&fs, loadBuffer, 1024, &file2);
    storfs_fputs(&fs, loadBuffer, 512, &file3);
    display_cache(fs);


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
    storfs_fopen(&fs, "C:/Testing/12.txt", "r+", &file1);
    storfs_fopen(&fs, "C:/Testing/123.txt", "a+", &file2);
    storfs_fopen(&fs, "C:/Testing/1234.txt", "a+", &file3);
    storfs_fopen(&fs, "C:/Testing/12345.txt", "w+", &file4);
    storfs_fputs(&fs, loadBuffer, 256, &file1);
    storfs_fputs(&fs, loadBuffer, 1023, &file2);
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
    storfs_fgets(&fs, buffer, 512, &file4);
    buffer[512] = '\0';
    LOGI(TAG, "File Read: %s", buffer);
    storfs_fopen(&fs, "C:/Testing/TEST/Pest/12.txt", "r+", &file1);
    storfs_fopen(&fs, "C:/Testing/TEST/Pest/123.txt", "a+", &file2);
    storfs_fputs(&fs, loadBuffer, 256, &file1);
    storfs_fputs(&fs, loadBuffer, 1024, &file2);

    for(int i = 21; i < 45; i++)
    {
        location.pageLoc = i;
        storfs_display_header(&fs, location);
    }

    //Reading from older files
    storfs_fgets(&fs, buffer, 256, &file1);
    buffer[256] = '\0';
    LOGI(TAG, "File Read: %s", buffer);
    storfs_fgets(&fs, buffer, 1024, &file2);
    buffer[1024] = '\0';
    i = 0;
    buffCount = 0;
    while(buffer[i++] != '\0')
    {
        buffCount++;
    }
    LOGI(TAG,"Buff count: %d \n", buffCount);
    LOGI(TAG, "File Read: %s", buffer);
    storfs_fgets(&fs, buffer, 100, &file3);
    buffer[100] = '\0';
    LOGI(TAG, "File Read: %s", buffer);
    for(int i = 0; i < 512; i++)
    {
        buffer[i] = 0;
    }
    storfs_fgets(&fs, buffer, 512, &file4);
    buffer[512] = '\0';
    LOGI(TAG, "File Read: %s", buffer);

    //Writing and reading from older files
    char *strTest = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Mauris interdum lacus dolor, sit amet aliquet dolor faucibus id. Sed ac lectus et diam rhoncus iaculis ut sed diam. Sed vel elit id sem sollicitudin maximus efficitur quis lacus. Pellentesque tristique enim et magna condimentum viverra. Phasellus erat neque, euismod a sapien vitae, auctor tempus diam. Sed ut elit erat. Aliquam dignissim tellus vitae hendrerit interdum. Aliquam convallis diam non nisi mollis, vitae eleifend sem tincidunt. Pellentesque ultrices in dolor et viverra. Maecenas nec dui eget ligula pharetra rutrum sit amet sed nunc.Vivamus aliquam lorem vel est egestas, vitae porttitor libero ultrices. Vivamus lacinia cursus dolor, quis ornare sem euismod fringilla. Nunc nisl ex, cursus et ligula quis, fringilla sodales mi. Nulla facilisi. Vestibulum dictum vel quam tristique vulputate.";
    storfs_fopen(&fs, "C:/Testing/12345.txt", "a+", &file4);
    storfs_fputs(&fs, strTest, 865, &file4);
    storfs_fgets(&fs, buffer, 512+865, &file4);
    LOGI(TAG, "File Read: %s", buffer);

    //Removing a directory
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
    storfs_fputs(&fs, strTest, 762, &file1);
    storfs_fgets(&fs, buffer, 762, &file1);
    buffer[762] = '\0';
    LOGI(TAG, "File Read: %s", buffer);
    storfs_fgets(&fs, buffer, 523, &file1);
    buffer[523] = '\0';
    LOGI(TAG, "File Read: %s", buffer);

    //Different size read than write test
    storfs_fputs(&fs, loadBuffer, 1024, &file1);
    storfs_fgets(&fs, buffer, 762, &file1);
    buffer[762] = '\0';
    LOGI(TAG, "File Read: %s", buffer);

    LOGI(TAG, "fileName %s \r\n \
    fileInfo %x \r\n \
    childLocation %lx%lx \r\n \
    siblingLocation %lx%lx \r\n \
    reserved %x \r\n \
    fragmentLocation/nextOpenByte %lx%lx \r\n \
    fileSize %lx \r\n \
    crc %x",fs.cachedInfo.rootHeaderInfo[0].fileName, fs.cachedInfo.rootHeaderInfo[0].fileInfo, \
    (uint32_t)(fs.cachedInfo.rootHeaderInfo[0].childLocation >> 32), (uint32_t)fs.cachedInfo.rootHeaderInfo[0].childLocation, \
    (uint32_t)(fs.cachedInfo.rootHeaderInfo[0].siblingLocation >> 32), (uint32_t)fs.cachedInfo.rootHeaderInfo[0].siblingLocation, \
    fs.cachedInfo.rootHeaderInfo[0].reserved, (uint32_t)(fs.cachedInfo.rootHeaderInfo[0].fragmentLocation >> 32), (uint32_t)fs.cachedInfo.rootHeaderInfo[0].fragmentLocation, \
    fs.cachedInfo.rootHeaderInfo[0].fileSize, fs.cachedInfo.rootHeaderInfo[0].crc);

}