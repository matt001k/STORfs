#ifndef __STORFS_CONFIG_H
#define __STORFS_CONFIG_H

#include <stdio.h>

#define  STORFS_MAX_FILE_NAME                 32

/* Logging defines */
#define STORFS_NO_LOG                       
#define STORFS_LOGI(TAG, fmt, ...)
#define STORFS_LOGD(TAG, fmt, ...)
#define STORFS_LOGW(TAG, fmt, ...)
#define STORFS_LOGE(TAG, fmt, ...)
#define STORFS_LOG_DISPLAY_HEADER       

#define STORFS_USE_CRC

#define STORFS_WEAR_LEVEL_RETRY_NUM
   
#endif