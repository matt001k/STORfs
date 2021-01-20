# STORfs 

STORfs is an open source flash file system for embedded MCUs inspired by easy portability, small footprint and well documentation. The middleware is written in C. Unlike other open source filesystems, the main goal of this project is to inform the user with as much information needed in order to port the file system into one of their projects.

STORfs supports

- Wear Levelling
- Directories and Files
- File size of 4GB
- Virtually unlimited storage space
- Easy user interface with functions


## Porting STORfs

The most basic needed structure for the file system is as follows:

``` c
storfs_t fs = {
    .read = storfs_read,			//Function to read a page from flash
    .write = storfs_write,			//Function to write a page in flash
    .erase = storfs_erase,			//Function to erase a page in flash
    .sync = storfs_sync,			//Sync after reading/writing to make certain the next command is ready
    .memInst = &memInst,			//Pointer to the memory instance of the flash chip driver (optional)
    .firstByteLoc = 0,				//Location of the first byte in storage to store the root partition
    .firstPageLoc = 0,				//Location of the first page in storage to store the root partition
    .pageSize = 512,				//Size of each erasable page(in bytes)
    .pageCount = 8191,				//Number of total pages in the file sstem
  };
```

Four functions are needed in order to port STORfs into a user's project:

```c
storfs_err_t (*read)(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size);

storfs_err_t (*write)(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size);

storfs_err_t (*erase)(const struct storfs *storfsInst, storfs_page_t page);

storfs_err_t (*sync)(const struct storfs *storfsInst);
```

- Read:
  - Used to read from a number of bytes (size) in memory at a specific page and byte location within that page
  - This function should not continuously read from multiple pages, must be one page at a time
- Write:
  - Used to write a number of bytes (size) in memory at a specific page and byte location within that page
  - This function should not be a continuous write to multiple pages, must be one page at a time
- Erase:
  - Erase a whole page from memory
- Sync:
  - Used to ensure the flash chip is ready for the next read/write/erase operation

Below is an **example** of initialization of the file system:

```c
storfs_err_t storfs_read(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
  at45xxxxx_main_mem_info_t mainMem;
  mainMem.memPageAddr = page;
  mainMem.memByteAddr = byte;

  if(at45_cont_arr_rx(storfsInst->memInst, mainMem, buffer, size) == AT45XXXXX_OK)
  {
    delay(50);	//Delay used in order to help synchronization
    return STORFS_OK;
  }
  return STORFS_ERROR;
}

storfs_err_t storfs_write(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
  at45xxxxx_err_t err = AT45XXXXX_OK;

  at45xxxxx_main_mem_info_t mainMem;
  mainMem.memPageAddr = page;
  mainMem.memByteAddr = byte;

  err = at45_buf_mem_pg_thru_tx_w_erase(storfsInst->memInst, mainMem, AT45XXXXX_SRAM_BUF_1, \
  		(uint8_t *)buffer, size);
  if(err == AT45XXXXX_OK)
  {
    delay(50);	//Delay used in order to help synchronization
    return STORFS_OK;
  }
  return STORFS_ERROR;
}

storfs_err_t storfs_erase(const struct storfs *storfsInst, storfs_page_t page)
{
  if(at45_page_erase(storfsInst->memInst, page) == AT45XXXXX_OK)
  {
    delay(50);	//Delay used in order to help synchronization
    return STORFS_OK;
  }
  return STORFS_ERROR;
}

storfs_err_t storfs_sync(const struct storfs *storfsInst)
{
  //Determine if the device is busy or ready for the next action
  if(device_busy_poll(storfsInst->memInst) == AT45XXXXX_OK)
  {
      return STORFS_OK;
  }
  return STORFS_ERROR;
}

int main(void)
{
    //Initialization of the flash memory device
    at45xxxxx_inst_t at45Inst;
   	at45Inst.cnfg.dataSize = AT45XXXXX_512B;
    at45Inst.spiInst = &SPIInst;
    at45_memory_init(&at45Inst);
    
    //Initialization structure of STORfs
    storfs_t fs = {
        .read = storfs_read,
        .write = storfs_write,
        .erase = storfs_erase,
        .sync = storfs_sync,
        .memInst = &at45Inst,
        .firstByteLoc = 0,
        .firstPageLoc = 20,
        .pageSize = 512,
        .pageCount = AT45XXXXX_MAX_PAGES,
      };
    
    //Mounting the file system into flash memory
    storfs_mount(&fs, "C:");
}
```



A custom CRC function may be used by defining the pre-processor definition: *STORFS_USE_CRC* that would be declared within the ```storfs_t``` structure:

``` C
storfs_err_t storfs_crc(const struct storfs *storfsInst, const uint8_t *buffer, storfs_size_t size)
{
    ...
}

storfs_t fs = {
    ...
    .crc = storfs_crc
    ...
}
```

*buffer* is the data to be calculated for the CRC and *size* will be the length in bytes of the CRC buffer.



Test scripts may be found in the *Test* and *Examples* folder above.

The test folder holds a program that will run off of a PC. Just use make to build the project and have a close look at how the filesystem works through the debugging messages.

Other examples are to test out STORfs on an MCU.



## Configuring STORfs

Configuration for STORfs can be found within the storfs_config.h file. 

Within this file a set of defines are used to declare certain functionalities within STORfs.

Further details below

``` C
#define  STORFS_MAX_FILE_NAME       32	//Maximum length for a file name, cannot be below 4 char

#define STORFS_NO_LOG 					//Used to determine whether or not STORfs will use logging
#define STORFS_USE_LOGD					//Will use logging debugging functionality  
	#define LOGD						//Function-like macro to define the logging mechanism
#define STORFS_USE_LOGI                	//Will use logging information functionality       
    #define LOGI						//Function-like macro to define the logging mechanism
#define STORFS_USE_LOGW					//Will use logging warning functionality  
    #define LOGW						//Function-like macro to define the logging mechanism
#define STORFS_USE_LOGE					//Will use logging error functionality  
    #define LOGE

#define STORFS_LOG_DISPLAY_HEADER		//Define to utilize the display headder logging functionality

#define STORFS_USE_CRC					//Define to use a custom user CRC check for wear-levelling
```


## STORfs Functions

``` c 
storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName);
```

- Mounts the file system at the dedicated partition name

- Partition name can be NULL if the file system is already instantiated
- All files branch from this partition

``` c
storfs_err_t storfs_mkdir(storfs_t *storfsInst, char *pathToDir);
```

- Makes a new directory in the according path

  - The path must branch from the root directory

    *Ex:* Path to a new directory *"C:/Users/Documents"* will create a directory called Documents as a child to Users

- Can create multiple directories within one path

``` c 
storfs_err_t storfs_touch(storfs_t *storfsInst, char *pathToFile);
```

- Makes a new file in the according path
- Only a single file can be made per call

``` C 
storfs_err_t storfs_fopen(storfs_t *storfsInst, char *pathToFile, const char * mode, STORFS_FILE *stream)
```

- Opens or creates a file at the according path

- File may be read or written to depending on the mode provided

  - **w:** write only and truncate existing file

  - **w+:** read/write and truncate existing file

  - **r:** read only

  - **r+:** read/write and truncate the existing file

  - **a:** write only and append existing file

  - **a+:** read/write and append existing file

``` c
storfs_err_t storfs_fputs(storfs_t *storfsInst, const char *str, const int n, STORFS_FILE *stream);
```
- Write to a file stream according to the flags used to open a file
``` c
storfs_err_t storfs_fgets(storfs_t *storfsInst, char *str, int n, STORFS_FILE *stream);
```
- Used to read from a file stream for a certain amount of characters
``` c
storfs_err_t storfs_rm(storfs_t *storfsInst, char *pathToFile, STORFS_FILE *stream);
```
- Removes a file/directory according to the path declard
- If  a file is associated with a stream, the stream may be used called to terminate its values
- If a directory is removed, all of its contents within(children) will be removed as well

## STORfs Explained

STORfs is laid out similar to a tree type data structure, utilizing children and siblings.

The following pictures how files/directories are laid out within the system: 

<div style="text-align:center"><img src="Documentation\images\STORfs_Layout.svg" /></div>

Files may only have siblings whereas directories have the ability to have children and siblings. 



### File Header Information

Each file is laid out with a header. The header consists of:

- File Information
- Filename
  - Max length is user defined(minimum max length is 4)
- Child Location
  - Points to child location
- Sibling Location
  - Points to sibling location
- File Fragment Location
  - Points to file fragment Location
- File Size
  - Up to 4GB
- CRC 
  - Can be user defined

An in depth look shown explained below:



### CRC Information

CRC is calculated differently depending on the type of item being stored in the file system.



The root and directories will calculate the CRC based on its filename along with very newly created files. 

*Ex:* If the root name is *C:* the CRC will be calculated based on the 2 bytes of the file name, this is depending on the formula provided by the user or the already designed CRC function in storfs.c



Files (previously created and either written to or appended to) will utilize the data written to its page/section to determine the CRC. Each fragment section will utilize the data written to that page/section to calculate the CRC of that fragment as well.



When writing to a file/fragment STORfs will determine whether or not that page/section is bad by reading the data just written to the page/section and calculating the CRC from the read data. If the CRC calculated does not match the header CRC, STORfs will find the next available page/section and check that section, this process goes on until a good page/section is available.



