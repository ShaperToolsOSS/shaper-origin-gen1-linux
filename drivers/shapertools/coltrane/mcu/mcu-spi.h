#ifndef _MCU_SPI_H_
#define _MCU_SPI_H_

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/spi/spi.h>
#include <linux/time64.h>

#define MCU_SPI_MODE_FLASH 1UL
#define MCU_SPI_MODE_BOOTLOADER 2UL
#define MCU_SPI_MODE_SRAM 3UL

#define MCU_SPI_BL_LOAD_FLASH 0x02UL
#define MCU_SPI_BL_LOAD_LAUNCH 0x08UL
#define MCU_SPI_BL_LOAD_VERIFY 0x10UL

#define MCU_SPI_INT_EVENT 0x00000001UL

typedef ssize_t (*mcu_spi_dev_attr_show_t)(struct device *dev, struct device_attribute *attr, char *buf);
typedef ssize_t (*mcu_spi_dev_attr_store_t)(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);

struct mcu_spi_device_attribute
{
	struct list_head node;
	struct device_attribute parent;
	void *context;
};

struct mcu_spi_device  {
	const char *name;
	const char *node_name;
	umode_t mode;
	const struct file_operations *ops;

	unsigned int flags;
	dev_t devnode;
	struct mcu_spi *parent;
	struct device *this;
	const struct attribute_group **groups;

	struct mutex lock;

	struct list_head event_node;
	struct list_head device_node;

	struct list_head attributes;
};

struct mcu_spi_event
{
	int (*notify)(unsigned int event, void *context);
	void *context;
	unsigned int event_mask;
	struct list_head node;
};

struct mcu_spi_transaction
{
	void *cmd;
	size_t size;
	void *payload;
	void *context;
	void (*done)(struct mcu_spi_transaction *trans, size_t count, int status);

	struct spi_message msg;
	struct spi_transfer xfers[2];

	struct timespec64 start;
	struct timespec64 end;
};

static inline struct mcu_spi_device_attribute *to_mcu_spi_device_attr(struct device_attribute *attr)
{
	return attr != 0 ? container_of(attr, struct mcu_spi_device_attribute, parent) : 0;
}

extern int mcu_spi_register(struct mcu_spi_device *mcu_spi_dev);
extern void mcu_spi_unregister(struct mcu_spi_device *mcu_spi_dev);

extern struct mcu_spi_device *mcu_spi_find(const char *name);
extern struct mcu_spi_device *mcu_spi_get(struct mcu_spi_device *mcu_spi_dev);
extern void mcu_spi_put(struct mcu_spi_device *mcu_spi_dev);

extern int mcu_spi_lock(struct mcu_spi_device *mcu_spi_dev, unsigned int flags);
extern void mcu_spi_unlock(struct mcu_spi_device *mcu_spi_dev);

extern int mcu_spi_chip_state(struct mcu_spi_device *mcu_spi_dev);
extern int mcu_spi_reset(struct mcu_spi_device *mcu_spi_dev, int mode);
extern int mcu_spi_setup(struct mcu_spi_device *mcu_spi_dev, unsigned int speed, unsigned char width, unsigned short mode);

extern int mcu_spi_load_image(struct mcu_spi_device *mcu_spi_dev, const char *name, int flags);
extern int mcu_spi_erase_pages(struct mcu_spi_device *mcu_spi_dev, int *pages, size_t count);
extern int mcu_spi_mass_erase(struct mcu_spi_device *mcu_spi_dev);
extern int mcu_spi_read_memory(struct mcu_spi_device *mcu_spi_dev, uint32_t addr, void *buffer, size_t count);
extern int mcu_spi_write_memory(struct mcu_spi_device *mcu_spi_dev, uint32_t addr, const void *buffer, size_t count);

extern int mcu_spi_pump(struct mcu_spi_device *mcu_spi_dev, const void *tx, void *rx, size_t count);
extern void mcu_spi_trans_init(struct mcu_spi_transaction *trans, void *cmd, size_t csize, void *payload, size_t psize, void (*done)(struct mcu_spi_transaction *trans, size_t count, int status));
extern int mcu_spi_async(struct mcu_spi_device *mcu_spi_dev, struct mcu_spi_transaction *trans);
extern int mcu_spi_sync(struct mcu_spi_device *mcu_spi_dev, void *cmd, size_t csize, void *payload, size_t psize);
extern int mcu_spi_send_reset(struct mcu_spi_device *mcu_spi_dev);

extern void mcu_spi_register_event(struct mcu_spi_device *mcu_spi_dev, struct mcu_spi_event *event);
extern void mcu_spi_unregister_event(struct mcu_spi_device *mcu_spi_dev, struct mcu_spi_event *event);

extern int mcu_spi_add_attr(struct mcu_spi_device *mcu_spi_dev, const char *name, mcu_spi_dev_attr_show_t show, mcu_spi_dev_attr_store_t store, void *context);
extern int mcu_spi_remove_attr(struct mcu_spi_device *mcu_spi_dev, const char *name);

#endif
