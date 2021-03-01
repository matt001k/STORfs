<div align="center"><h1>
    STORfs Open Source File System</h1> <br>
    <h3>
        Release Version 1.0.2 <br><br>
    	Created by: KrauseGLOBAL Solutions, LLC <br><br>
    </h3></div>


## What is STORfs?

STORfs is an open source flash file system for embedded MCUs inspired by easy portability, small footprint and well documentation. The middleware is written in C. Unlike other open source filesystems, the main goal of this project is to inform the user with as much information needed in order to port the file system into one of their projects.

STORfs supports:

- Wear Levelling
- Directories and Files
- File size of 4GB
- Virtually unlimited storage space
- Easy user interface with functions
- Small footprint
  - Compiling as low as 5764 Bytes (*compiled with gcc arm7*)


## Porting STORfs

STORfs makes the assumption:
- Read/Write and Erase operations are of the same size
  - ex: If the erase operation of the storage device is 4kB then the operations of reading/writing will be at that same value the user must modify the function pointers associated correctly to accommodate if they are not the same size

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



Test scripts may be found in the *Examples* folder above.

The test folder holds a program that will run off of a PC under the folder *test*. Just use make to build the project and have a close look at how the file system works through the debugging messages.

Other examples are to test out STORfs on an MCU.



## Configuring STORfs

Configuration for STORfs can be found within the storfs_config.h file. 

Within this file a set of defines are used to declare certain functionalities within STORfs.

Further details below

``` C
#define  STORFS_MAX_FILE_NAME       32	//Maximum length for a file name, cannot be below 4 char

#define STORFS_NO_LOG 					//Used to determine whether or not STORfs will use logging
#define STORFS_LOGI(TAG, fmt, ...)		//Function-like macro to define the logging mechanism      
#define STORFS_LOGD(TAG, fmt, ...)		//Function-like macro to define the logging mechanism
#define STORFS_LOGW(TAG, fmt, ...)		//Function-like macro to define the logging mechanism
#define STORFS_LOGE(TAG, fmt, ...)		//Function-like macro to define the logging mechanism

#define STORFS_LOG_DISPLAY_HEADER		//Define to utilize the display headder logging functionality

#define STORFS_USE_CRC					//Define to use a custom user CRC check for wear-levelling

#define STORFS_WEAR_LEVEL_RETRY_NUM   //Defines the number of retries that a write operation will try to write to a page before enabling wear-levelling functionality
```


## STORfs Functions

``` c 
storfs_err_t storfs_mount(storfs_t *storfsInst, char *partName);
```

- Mounts the file system at the dedicated partition name
- File system must always be mounted before using the file system, even if the file system is already instantiated
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
- Removes a file/directory according to the path declared
- If  a file is associated with a stream, the stream may be used called to terminate its values
- If a directory is removed, all of its contents within(children) will be removed as well

## STORfs Explained

STORfs is laid out similar to a tree type data structure, utilizing children and siblings.

The following pictures how files/directories are laid out within the system: 

<div style="text-align:center"><img src="Documentation\images\STORfs_Layout.svg" /></div>

Files may only have siblings whereas directories have the ability to have children and siblings. Each item in the file system is connected to one another via a header which has information about the items child/sibling/fragment location (depending on item's type).



### File Header Information and File Layout

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



At the beginning of the file system, two separate pages/blocks are dedicated to the root partition file header. Two are used just in case reading data from the first file header is corrupted, the data may then be read from the second header.

Files are fragmented at the end of a page/block. When the page size is exceeded, data will be fragmented to the next open page/block. A new smaller sized header will be written to the beginning of that page/block followed by the remaining data, this is repeated until the amount of data to be written to the device is exhausted.

An in depth look is shown below:

<div style="text-align:center"><img src="Documentation\images\STORFS_Scheme.svg" /></div>

As can be seen, fragment headers of files are much smaller as to allow for as much storage space as possible for a file.

### ROOT Directory

The root directory has information pertaining to the next available byte to write to within the storage space. This information is cached within the file systems main structure `storfs_t` and is updated whenever files are added, or removed from the system. This way, STORfs knows where next to write a file on the fly. Having this information written to root directory header ensures that even on power down, when mounting the file system again on boot, this information will be easily grabbed and applied to that cache.

### CRC Information

CRC is calculated differently depending on the type of item being stored in the file system.

The root and directories will calculate the CRC based on its **filename** along with very newly created files. 

*Ex:* If the root name is *C:* the CRC will be calculated based on the 2 bytes of the file name, this is depending on the formula provided by the user or the already designed CRC function in storfs.c

Files (previously created and either written to or appended to) will utilize the **data written to its page/block** to determine the CRC. Each fragment section will utilize the data written to that page/block to calculate the CRC of that fragment as well.

When writing to a file/fragment STORfs will determine whether or not that page/block is bad by reading the data just written to the page/section and calculating the CRC from the read data. If the CRC calculated does not match the header CRC, STORfs will find the next available page/section and check that section, this process goes on until a good page/section is available. This is how wear-levelling is implemented in STORfs.



## STORfs Compiled Sizes

| Compiler | Size (in bytes) |
| :------: | :-------------: |
| GCC ARM7 |      5764       |
| Others to Come Soon... | |


## Tested Hardware
STORfs has been tested and validated on the following hardware:
- Adesto
  - AT25SF321B
  - AT45DB321E


## Other Information Regarding STORfs

- A software overview can be found in Documentation/software_overview including flowcharts and other information regarding the design of the functions within STORfs
- Contributing guidelines can be found under the documentation directory
- Examples can be found under the examples directory
  - Example to be ran on a PC is under the directory test
  - MCU application testing is found under:
    - function_compatibility
    - mcu_application
- Revision updates may be found under Documentation/revision_notes



## Future Plans for STORfs

- An option to include no directories, just files under the root partition
- Journalling System
- Smart relocatable root headers (further wear-handling)
- Added functions to interface with storage device



