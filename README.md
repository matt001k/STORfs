# STORfs 

STORfs is an open source flash file system for embedded MCUs inspired by easy portability and well documentation. The middleware is written in C. Unlike other open source filesystems, the main goal of this project is to inform the user with as much information needed in order to port the file system into one of their projects.



## Porting STORfs

The most basic needed structure for the file system is as follows:

``` c
storfs_t fs = {
    .read = storfs_read,			//Function to read a page from flash
    .write = storfs_write,			//Function to write a page in flash
    .erase = storfs_erase,			//Function to erase a page in flash
    .sync = storfs_sync,			//Sync after reading/writing to make certain the next command is ready
    .memInst = &memInst,			//Pointer to the memory instance of the flash chip driver (can be NULL)
    .firstByteLoc = 0,				//Location of the first byte in storage to store the root partition
    .firstPageLoc = 0,				//Location of the first page in storage to store the root partition
    .pageSize = 512,				//Size of each erasable page(in bytes)
    .pageCount = 8191,				//Number of total pages in the file sstem
  };
```

Four functions are needed in order to port STORfs into a users project:

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

Below is an example of initialization of the file system:

```c
storfs_err_t storfs_read(const struct storfs *storfsInst, storfs_page_t page,
            storfs_byte_t byte, uint8_t *buffer, storfs_size_t size)
{
  at45xxxxx_main_mem_info_t mainMem;
  mainMem.memPageAddr = page;
  mainMem.memByteAddr = byte;

  if(at45_cont_arr_rx(&at45Inst, mainMem, buffer, size) == AT45XXXXX_OK)
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

  err = at45_buf_mem_pg_thru_tx_w_erase(&at45Inst, mainMem, AT45XXXXX_SRAM_BUF_1, (uint8_t *)buffer, size);
  if(err == AT45XXXXX_OK)
  {
    delay(50);	//Delay used in order to help synchronization
    return STORFS_OK;
  }

  LOGE(TAG, "Major Error");
  return STORFS_ERROR;
}

storfs_err_t storfs_erase(const struct storfs *storfsInst, storfs_page_t page)
{
  if(at45_page_erase(&at45Inst, page) == AT45XXXXX_OK)
  {
    delay(50);	//Delay used in order to help synchronization
    return STORFS_OK;
  }

  return STORFS_ERROR;
}

storfs_err_t storfs_sync(const struct storfs *storfsInst)
{
  //Determine if the device is busy or ready for the next action
  if(device_busy_poll(&at45Inst) == AT45XXXXX_OK)
  {
      return STORFS_OK;
  }

  return STORFS_ERROR;
}

int main(void)
{
    //Initialization of the flash memory device
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



Test scripts may be found in the *Test* and *Examples* folder above.

The test folder holds a program that will run off of a PC. Just use make to build the project and have a close look at how the filesystem works through the debugging messages.

Other examples are to test out STORfs on an MCU.



## STORfs Explained

STORfs was laid out similar to a tree type data structure, utilizing children and siblings.

The following pictures how files/directories are laid out within the system: 

```mermaid
	
```

