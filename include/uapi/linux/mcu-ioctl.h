#ifndef _MCU_IOCTL_H_
#define _MCU_IOCTL_H_

#include <linux/ioctl.h>

#define MCU_MODE_FLASH 1UL
#define MCU_MODE_BOOTLOADER 2UL
#define MCU_MODE_SRAM 3UL

#define MCU_BL_LOAD_FLASH 0x02UL
#define MCU_BL_LOAD_LAUNCH 0x08UL
#define MCU_BL_LOAD_VERIFY 0x10UL

#define MCU_BUFFER_USERSPACE 0x00000000UL
#define MCU_BUFFER_MMAP 0x00000001UL
#define MCU_BUFFER_GLOBAL 0x00000002UL
#define MCU_BUFFER_LOCAL 0x00000004UL

#define _MCU_IOCTL_TYPE 0xfe

#define _MCU_IOCTL_CMD_RESET 0x01UL
#define _MCU_IOCTL_CMD_STATE 0x02UL
#define _MCU_IOCTL_CMD_SETUP_SPI 0x03UL

#define _MCU_IOCTL_CMD_LOAD 0x20UL
#define _MCU_IOCTL_CMD_ERASE_PAGES 0x21UL
#define _MCU_IOCTL_CMD_MASS_ERASE 0x22UL

#define _MCU_IOCTL_CMD_PUMP 0x30UL

#define _MCU_IOCTL_CMD_RAW 0x40UL

#define _MCU_IOCTL_CMD_GET_NUM_DESC 0x50UL
#define _MCU_IOCTL_CMD_GET_DESC_LIST 0x51UL
#define _MCU_IOCTL_CMD_GET_DESC 0x52UL
#define _MCU_IOCTL_CMD_SPI_RESET 0x53UL

#define _MCU_IOCTL_CMD_MODE 0x54

#define _MCU_IOCTL_CMD_CAPTURE 0X55
#define _MCU_IOCTL_CMD_FLUSH 0X56
#define _MCU_IOCTL_CMD_SPACE_REQUEST_BUF 0x57
#define _MCU_IOCTL_CMD_SPACE_RELEASE_BUF 0x58

#define MCU_DESC_MAX_NAME_SIZE 64
#define MCU_FLASH_BLOCK_SIZE 256
#define MCU_FIRMWARE_PATH_LEN 256UL
#define MCU_PATH_MAX_PAGESET 8

enum mcu_desc_field_type
{
	MCU_DESC_FIELD_TYPE_VOID = -1,

	MCU_DESC_FIELD_TYPE_FLOAT = 0,
	MCU_DESC_FIELD_TYPE_DOUBLE = 1,
	MCU_DESC_FIELD_TYPE_CHAR = 2,
	MCU_DESC_FIELD_TYPE_INT8 = 3,
	MCU_DESC_FIELD_TYPE_UINT8 = 4,
	MCU_DESC_FIELD_TYPE_INT16 = 5,
	MCU_DESC_FIELD_TYPE_UINT16 = 6,
	MCU_DESC_FIELD_TYPE_INT32 = 7,
	MCU_DESC_FIELD_TYPE_UINT32 = 8,
	MCU_DESC_FIELD_TYPE_INT64 = 9,
	MCU_DESC_FIELD_TYPE_UINT64 = 10,

	MCU_DESC_FIELD_TYPE_FLOAT_ARRAY = 11,
	MCU_DESC_FIELD_TYPE_DOUBLE_ARRAY = 12,
	MCU_DESC_FIELD_TYPE_CHAR_ARRAY = 13,
	MCU_DESC_FIELD_TYPE_INT8_ARRAY = 14,
	MCU_DESC_FIELD_TYPE_UINT8_ARRAY = 15,
	MCU_DESC_FIELD_TYPE_INT16_ARRAY = 16,
	MCU_DESC_FIELD_TYPE_UINT16_ARRAY = 17,
	MCU_DESC_FIELD_TYPE_INT32_ARRAY = 18,
	MCU_DESC_FIELD_TYPE_UINT32_ARRAY = 19,
	MCU_DESC_FIELD_TYPE_INT64_ARRAY = 20,
	MCU_DESC_FIELD_TYPE_UINT64_ARRAY = 21,

	MCU_DESC_FIELD_TYPE_FLOAT_TRIPLE = 22,
	MCU_DESC_FIELD_TYPE_DOUBLE_TRIPLE = 23,
	MCU_DESC_FIELD_TYPE_INT8_TRIPLE = 24,
	MCU_DESC_FIELD_TYPE_UINT8_TRIPLE = 25,
	MCU_DESC_FIELD_TYPE_INT16_TRIPLE = 26,
	MCU_DESC_FIELD_TYPE_UINT16_TRIPLE = 27,
	MCU_DESC_FIELD_TYPE_INT32_TRIPLE = 28,
	MCU_DESC_FIELD_TYPE_UINT32_TRIPLE = 29,
	MCU_DESC_FIELD_TYPE_INT64_TRIPLE = 30,
	MCU_DESC_FIELD_TYPE_UINT64_TRIPLE = 31,

	MCU_DESC_FIELD_TYPE_LAST = 31,

};

struct mcu_ioctl_setup_spi
{
	unsigned int speed_hz;
	unsigned char bits_per_word;
	unsigned short mode;
};

struct mcu_ioctl_load
{
	int flags;
	char path[MCU_FIRMWARE_PATH_LEN];
};

struct mcu_ioctl_pageset
{
	int num_pages;
	int page_set[MCU_PATH_MAX_PAGESET];
};

struct mcu_ioctl_pump
{
	void *tx;
	void *rx;
	size_t size;
};

struct mcu_ioctl_raw
{
	void *cmd;
	size_t csize;
	void *payload;
	size_t psize;
};

struct mcu_ioctl_desc_info
{
	char desc_name[MCU_DESC_MAX_NAME_SIZE];
	unsigned int desc_id;
	size_t desc_size;
};

struct mcu_ioctl_field_desc
{
	char name[MCU_DESC_MAX_NAME_SIZE];
	enum mcu_desc_field_type type;
	size_t size;
	size_t offset;
};

struct mcu_ioctl_space_desc
{
	char name[MCU_DESC_MAX_NAME_SIZE];
	unsigned int id;
	size_t size;
	int num_fields;
	struct mcu_ioctl_field_desc fields[];
};

struct mcu_ioctl_buffer
{
	unsigned int flags;
	off_t offset;
	size_t size;
#ifdef __KERNEL__
	struct timespec64 timestamp;
#else
	struct timespec timestamp;
#endif
	char *buffer;
};

#define MCU_IOCTL_RESET _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_RESET, unsigned long)
#define MCU_IOCTL_STATE _IOR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_STATE, unsigned long *)
#define MCU_IOCTL_SETUP_SPI _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_SETUP_SPI, struct mcu_ioctl_setup_spi *)

#define MCU_IOCTL_LOAD _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_LOAD, struct mcu_ioctl_load *)
#define MCU_IOCTL_ERASE_PAGES _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_ERASE_PAGES, struct mcu_ioctl_pageset *)
#define MCU_IOCTL_MASS_ERASE _IO(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_MASS_ERASE)

#define MCU_IOCTL_PUMP _IOWR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_PUMP, struct mcu_ioctl_pump *)

#define MCU_IOCTL_RAW _IOWR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_RAW, struct mcu_ioctl_raw *)

#define MCU_IOCTL_GET_NUM_DESC _IOR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_GET_NUM_DESC, unsigned long *)
#define MCU_IOCTL_GET_DESC_LIST _IOR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_GET_DESC_LIST, struct mcu_ioctl_desc_info *)
#define MCU_IOCTL_GET_DESC _IOR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_GET_DESC, struct mcu_ioctl_space_desc *)

#define MCU_IOCTL_SPI_RESET _IO(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_SPI_RESET)

#define MCU_IOCTL_MODE _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_MODE, unsigned long)

#define MCU_IOCTL_SPACE_REQUEST_BUF _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_SPACE_REQUEST_BUF, struct mcu_ioctl_buffer *)
#define MCU_IOCTL_SPACE_RELEASE_BUF _IOW(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_RELEASE_BUF,_SPACE, struct mcu_ioctl_buffer *)
#define MCU_IOCTL_CAPTURE _IOWR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_CAPTURE, struct mcu_ioctl_buffer *)
#define MCU_IOCTL_FLUSH _IOWR(_MCU_IOCTL_TYPE, _MCU_IOCTL_CMD_FLUSH, struct mcu_ioctl_buffer *)

#endif
