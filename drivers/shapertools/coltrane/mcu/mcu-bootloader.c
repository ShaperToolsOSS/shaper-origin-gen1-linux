#define DEBUG

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/mcu-ioctl.h>

#include <asm/uaccess.h>

#include "mcu-spi.h"

struct mcu_bootloader_file
{
	struct mcu_spi_device *mcu_spi_dev;
	struct mutex lock;
};

static int mcu_bootloader_open(struct inode *inode, struct file *file)
{
	int status;
	struct mcu_spi_device *mcu_spi_dev = file->private_data;
	struct mcu_bootloader_file *mcu_file;

	/* We force single open via the mcu api */
	if (!mcu_spi_lock(mcu_spi_dev, O_EXCL | O_NONBLOCK))
		return -EBUSY;

	/* Put into boot loader mode */
	status = mcu_spi_reset(mcu_spi_dev, MCU_MODE_BOOTLOADER);
	if (status < 0)
		goto error_unlock_device;

	/* Allocate the the new file structure */
	mcu_file = kzalloc(sizeof(struct mcu_bootloader_file), GFP_KERNEL);
	if (!mcu_file) {
		status = -ENOMEM;
		goto error_unlock_device;
	}

	/* Initialize the lock */
	mutex_init(&mcu_file->lock);

	/* Save the device */
	mcu_file->mcu_spi_dev = mcu_spi_dev;

	/* Update the file private data */
	file->private_data = mcu_file;

	/* All good */
	return 0;

error_unlock_device:
	mcu_spi_unlock(mcu_spi_dev);

	return status;
}

static int mcu_bootloader_release(struct inode *inode, struct file *file)
{
	struct mcu_bootloader_file *mcu_file = file->private_data;

	/* Unlock the device */
	mcu_spi_unlock(mcu_file->mcu_spi_dev);

	/* Release the file */
	kfree(mcu_file);

	/* Always good */
	return 0;
}

static ssize_t mcu_bootloader_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	ssize_t read_size;
	ssize_t amount = 0;
	uint8_t buffer[MCU_FLASH_BLOCK_SIZE];

	struct mcu_bootloader_file *mcu_file = file->private_data;

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

	/* Try to read everything requested in multiples of the block size */
	while (amount < count) {

		/* How should we read?*/
		read_size = count - amount < MCU_FLASH_BLOCK_SIZE ? count - amount : MCU_FLASH_BLOCK_SIZE;

		/* Forward */
		read_size = mcu_spi_read_memory(mcu_file->mcu_spi_dev, *offset, buffer, read_size);
		if (read_size < 0) {
			amount = read_size;
			break;
		}

		/* Copy to the user buffer */
		if (copy_to_user(buf + amount, buffer, read_size)) {
			amount = -EFAULT;
			break;
		}

		/* Update the amount and position */
		amount += read_size;
		*offset += read_size;
	}

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	/* Return the amount read or error */
	return amount;
}

static ssize_t mcu_bootloader_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	ssize_t write_size;
	ssize_t amount = 0;
	uint8_t buffer[MCU_FLASH_BLOCK_SIZE];

	struct mcu_bootloader_file *mcu_file = file->private_data;

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

	/* Try to read everything requested in multiples of the block size */
	while (amount < count) {

		/* How should we write?*/
		write_size = count - amount < MCU_FLASH_BLOCK_SIZE ? count - amount : MCU_FLASH_BLOCK_SIZE;

		/* Copy data into buffer */
		if (copy_from_user(buffer, buf + amount, write_size)) {
			amount = -EFAULT;
			break;
		}

		/* Forward */
		write_size = mcu_spi_write_memory(mcu_file->mcu_spi_dev, *offset, buffer, write_size);
		if (write_size < 0) {
			amount = write_size;
			break;
		}

		/* Update the amount and position */
		amount += write_size;
		*offset += write_size;
	}

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	/* Return the amount read or error */
	return amount;
}

static loff_t mcu_bootloader_llseek(struct file *file, loff_t offset, int whence)
{
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;

	case SEEK_CUR:
		newpos = file->f_pos + offset;
		break;

	case SEEK_END:
		newpos = 0xffffffff;
		break;

	default:
		return -EINVAL;
	}

	/* Check the new position */
	if (newpos < 0)
		return -EINVAL;

	/* Update the file position */
	file->f_pos = newpos;

	return newpos;
}

static long mcu_bootloader_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int status = -EINVAL;
	struct mcu_bootloader_file *mcu_file = file->private_data;
	struct mcu_ioctl_load ioctl_load;
	struct mcu_ioctl_pageset ioctl_pageset;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != _MCU_IOCTL_TYPE)
		return -ENOTTY;

	/* Check direction */
	if ((_IOC_DIR(cmd) & _IOC_READ) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	/* Single threaded */
	mutex_lock(&mcu_file->lock);

	/* Dispatch */
	switch (_IOC_NR(cmd)) {

	case _MCU_IOCTL_CMD_RESET:

		/* Range check and forward*/
		if (arg <= MCU_MODE_BOOTLOADER)
			status = mcu_spi_reset(mcu_file->mcu_spi_dev, (int)arg);
		else
			status = -EINVAL;

		break;

	case _MCU_IOCTL_CMD_LOAD:

		/* Extract the load data */
		if (copy_from_user(&ioctl_load, (struct mcu_ioctl_load __user *)arg, sizeof(struct mcu_ioctl_load)) == 0)
			status = mcu_spi_load_image(mcu_file->mcu_spi_dev, ioctl_load.path, ioctl_load.flags);
		else
			status = -EFAULT;

		break;

	case _MCU_IOCTL_CMD_MASS_ERASE:

		/* Forward */
		status = mcu_spi_mass_erase(mcu_file->mcu_spi_dev);

		break;

	case _MCU_IOCTL_CMD_ERASE_PAGES:

		/* Extract the load data */
		if (copy_from_user(&ioctl_pageset, (struct mcu_ioctl_pageset __user *)arg, sizeof(struct mcu_ioctl_pageset)) == 0)
			status = mcu_spi_erase_pages(mcu_file->mcu_spi_dev, ioctl_pageset.page_set, ioctl_pageset.num_pages);
		else
			status = -EFAULT;

		break;

	default:
		break;
	}

	/* All done */
	mutex_unlock(&mcu_file->lock);
	return status;
}

const struct file_operations mcu_bootloader_fops =
{
	.open = mcu_bootloader_open,
	.release = mcu_bootloader_release,
	.read = mcu_bootloader_read,
	.write = mcu_bootloader_write,
	.llseek = mcu_bootloader_llseek,
	.unlocked_ioctl	= mcu_bootloader_ioctl,
};

static __exit void mcu_bootloader_exit(void)
{
	/* Retrieve the device from the shim */
	struct mcu_spi_device *mcu_spi_dev = mcu_spi_find("bootloader");

	/* Unregister */
	mcu_spi_unregister(mcu_spi_dev);

	/* Free it */
	kfree(mcu_spi_dev);
}

static __init int mcu_bootloader_init(void)
{
	int status = 0;
	struct mcu_spi_device *mcu_spi_dev;

	/* Allocate the device */
	mcu_spi_dev = kzalloc(sizeof(struct mcu_spi_device), GFP_KERNEL);
	if (!mcu_spi_dev)
		return -ENOMEM;

	/* Initialize the device */
	mcu_spi_dev->name = "bootloader";
	mcu_spi_dev->ops = &mcu_bootloader_fops;

	/* Register the new device */
	status = mcu_spi_register(mcu_spi_dev);
	if (status < 0) {
		kfree(mcu_spi_dev);
		return status;
	}

	/* All good */
	return status;
}

module_init(mcu_bootloader_init);
module_exit(mcu_bootloader_exit);

MODULE_DESCRIPTION("Shaper Tool MCU Bootloader driver");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com");
MODULE_LICENSE("GPL");

