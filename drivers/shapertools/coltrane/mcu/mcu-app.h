#ifndef _MCU_APP_H_
#define _MCU_APP_H_

#include <asm/atomic.h>

#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/mcu-ioctl.h>

#include "mcu-spi.h"

#define MCU_APP_MAX_SPACES 64
#define MCU_APP_SPACE_ID 0x00000001
#define MCU_APP_INTERRUPT_ENABLE ((MCU_APP_SPACE_ID << 24UL) | 0x00000020UL)
#define MCU_APP_INTERRUPT_DISABLE ((MCU_APP_SPACE_ID << 24UL) | 0x00000028UL)
#define MCU_APP_INTERRUPT_SET ((MCU_APP_SPACE_ID << 24UL) | 0x00000030UL)
#define MCU_APP_INTERRUPT_CLEAR ((MCU_APP_SPACE_ID << 24UL) | 0x00000038UL)

#define MCU_APP_TO_SPACE_NUM(ID) ((ID >> 24UL) & 0xffUL)
#define MCU_APP_TO_SPACE_ADDR(ID) (ID & 0xffffff)

#define MCU_APP_QUEUE_SIZE 16

#define CMD_STATUS 0x01UL
#define CMD_READ 0x02UL
#define CMD_WRITE 0x04UL

struct field_desc;
struct space_descriptor;
struct mcu_descriptor;

struct mcu_app_cmd_block
{
	uint8_t flags;
	uint8_t space;
	uint16_t size;
	uint32_t addr;
};

struct space_map
{
	int num_mappings;
	struct space_descriptor *desc;
	void *buffer;
};

struct mcu_app_device
{
	struct mcu_spi_device mcu_spi_dev;
	struct mcu_spi_event mcu_event;
	struct mcu_descriptor *descriptor;
	spinlock_t descriptor_lock;

	/* TODO Need a better data structure to reduce memory usage, but this is really fast */
	struct {
		spinlock_t waiters_lock;
		struct list_head waiters;
	} stream_waiters[MCU_APP_MAX_SPACES];

	size_t space_buffer_size;
	struct kmem_cache *space_cache;

	struct mutex space_maps_lock;
	struct space_map space_maps[MCU_APP_MAX_SPACES];

	struct
	{
		uint64_t space_status;
		uint64_t interrupt_pending;
		uint64_t interrupt_enabled;
		uint64_t space_active;
	} mcu_status;
};

struct space_buffer
{
	struct kref ref;
	struct mcu_app_device *dev;

	unsigned int space_id;
	unsigned int space_size;
	unsigned int addr;

	int retries;
	struct mcu_app_cmd_block cmd;
	struct mcu_spi_transaction trans;
	uint64_t data[];
};

struct space_buffer_queue
{
	spinlock_t fifo_lock;
	DECLARE_KFIFO(fifo, struct space_buffer *, MCU_APP_QUEUE_SIZE);
};

struct mcu_app_file
{
	struct mcu_app_device *mcu_app_dev;
	struct mutex lock;
	void *bounce;

	unsigned int bound_space;
	struct space_descriptor *space_desc;

	struct space_buffer_queue input_queue;
	wait_queue_head_t ready_wq;
	struct list_head queue_node;

	struct space_map space_maps[MCU_APP_MAX_SPACES];

	ssize_t (*read)(struct file *file, char __user *buf, size_t count, loff_t *offset);
	loff_t (*llseek)(struct file *file, loff_t offset, int whence);
};


static inline struct mcu_app_device *to_mcu_app_device(struct mcu_spi_device *mcu_spi_dev)
{
	return mcu_spi_dev != 0 ? container_of(mcu_spi_dev, struct mcu_app_device, mcu_spi_dev) : 0;
}

int mcu_app_mmap(struct file *file, struct vm_area_struct *vma);
int mcu_app_mmap_capture(struct file *file, unsigned int flags, unsigned int space_id, size_t offset, size_t len, struct timespec64 *timestamp);
int mcu_app_mmap_flush(struct file *file, unsigned int flags, unsigned int space_id, size_t offset, size_t len, struct timespec64 *timestamp);

int mcu_app_capture(struct mcu_app_device *mcu_app_dev, unsigned int space, size_t offset, void *buffer, size_t len, struct timespec64 *timestamp);
int mcu_app_flush(struct mcu_app_device *mcu_app_dev, unsigned int space, size_t offset, const void *buffer, size_t len, struct timespec64 *timestamp);

static inline int mcu_app_read(struct mcu_app_device *mcu_app_dev, unsigned int space, size_t offset, void *buffer, size_t len)
{
	return mcu_app_capture(mcu_app_dev, space, offset, buffer, len, 0);
}

static inline int mcu_app_write(struct mcu_app_device *mcu_app_dev, unsigned int space, size_t offset, const void *buffer, size_t len)
{
	return mcu_app_flush(mcu_app_dev, space, offset, buffer, len, 0);
}

#endif
