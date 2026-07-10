#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <linux/crc32.h>
#include <linux/completion.h>
#include <linux/gpio/consumer.h>

#include "mcu-spi.h"

#define STM32F411VE_NUM_PAGES 8
#define STM32F411VE_ERASE_TIME_16KB 500
#define STM32F411VE_ERASE_TIME_64KB 1100
#define STM32F411VE_ERASE_TIME_128KB 2000
#define STM32F411VE_MASS_ERASE_TIME 8000
#define STM32F411VE_FLASH_START 0x08000000

#define STM32F4_BOOTLOADER_BLOCK_SIZE 256

#define STM32F4_BOOTLOADER_SOF 0x5aUL
#define STM32F4_BOOTLOADER_SYNC 0xa5UL
#define STM32F4_BOOTLOADER_ACK 0x79UL
#define STM32F4_BOOTLOADER_NACK 0x1fUL

#define STM32F4_BOOTLOADER_GET 0x00
#define STM32F4_BOOTLOADER_GET_VERSION 0x01
#define STM32F4_BOOTLOADER_GET_ID 0x02
#define STM32F4_BOOTLOADER_READ_MEMORY 0x11
#define STM32F4_BOOTLOADER_GO 0x21
#define STM32F4_BOOTLOADER_WRITE_MEMORY 0x31
#define STM32F4_BOOTLOADER_ERASE 0x44
#define STM32F4_BOOTLOADER_WRITE_PROTECT 0x63
#define STM32F4_BOOTLOADER_WRITE_UNPROTECT 0x73
#define STM32F4_BOOTLOADER_READOUT_PROTECT 0x82
#define STM32F4_BOOTLOADER_READOUT_UNPROTECT 0x92

#define STM32F4_BOOTLOADER_CAP_GET 0
#define STM32F4_BOOTLOADER_CAP_GET_VERSION 1
#define STM32F4_BOOTLOADER_CAP_GET_ID 2
#define STM32F4_BOOTLOADER_CAP_READ_MEMORY 3
#define STM32F4_BOOTLOADER_CAP_GO 4
#define STM32F4_BOOTLOADER_CAP_WRITE_MEMORY 5
#define STM32F4_BOOTLOADER_CAP_ERASE 6
#define STM32F4_BOOTLOADER_CAP_WRITE_PROTECT 7
#define STM32F4_BOOTLOADER_CAP_WRITE_UNPROTECT 8
#define STM32F4_BOOTLOADER_CAP_READOUT_PROTECT 9
#define STM32F4_BOOTLOADER_CAP_READOUT_UNPROTECT 10

#define STM32F4_BOOTLOADER_RETRIES 500UL

#define BOOTLOADER_FORCE_PIO 0
#define BOOTLOADER_SPI_SPEED_HZ 8000000
#define BOOTLOADER_SPI_WIDTH 8
#define BOOTLOADER_SPI_MODE SPI_MODE_0

#define FLASH_FORCE_PIO 0
#define FLASH_SPI_SPEED_HZ 20000000
#define FLASH_SPI_WIDTH 16
#define FLASH_SPI_MODE SPI_MODE_0

#define DEVICE_READY mcu_spi_wait_for_state

static int mcu_spi_wait_for_state(struct spi_device *spi, bool ready);

struct mcu_spi
{
	struct gpio_desc *gpio_mcu_reset;
	struct gpio_desc *gpio_mcu_boot;
	struct gpio_desc *gpio_mcu_int;
	struct gpio_desc *gpio_mcu_ready;
	struct gpio_descs *gpio_mcu_board_id;

	int int_irq_num;
	int ready_irq_num;
	int chip_state;
	struct spi_device *spi;

	struct mutex devices_lock;
	struct mutex function_lock;
	struct rw_semaphore mode_lock;
	struct mutex event_lock;

	struct list_head event_handlers;
	struct list_head childern;

	wait_queue_head_t mcu_ready_wq;

	unsigned long long pump_cnt;

	struct class *mcu_class;
	int mcu_major;
	int mcu_minor;
};

static const int page_erase_times[STM32F411VE_NUM_PAGES] =
{
	STM32F411VE_ERASE_TIME_16KB,
	STM32F411VE_ERASE_TIME_16KB,
	STM32F411VE_ERASE_TIME_16KB,
	STM32F411VE_ERASE_TIME_16KB,
	STM32F411VE_ERASE_TIME_64KB,
	STM32F411VE_ERASE_TIME_128KB,
	STM32F411VE_ERASE_TIME_128KB,
	STM32F411VE_ERASE_TIME_128KB,
};

static uint8_t stm32f4_xor_checksum(uint8_t checksum, const void *buf, size_t count)
{
	int i;
	const uint8_t *data = buf;

	/* XOR all the data */
	for (i = 0; i < count; ++i)
		checksum ^= data[i];

	return checksum;
}

static int stm32f4_wait_ack(struct mcu_spi *mcu_spi)
{
	int result;
	uint8_t value = 0;
	uint8_t response = 0;
	int retries = STM32F4_BOOTLOADER_RETRIES;
	struct spi_transfer xfer = { .tx_buf = &value, .rx_buf = &response, .len = 1};

	/* Send initial read */
	value = 0;
	response = 0;
	result = spi_sync_transfer(mcu_spi->spi, &xfer, 1);
	if (result < 0)
		return result;

	/* Wait for ack */
	do {

		/* Read the response */
		value = 0;
		response = 0;
		result = spi_sync_transfer(mcu_spi->spi, &xfer, 1);
		if (result < 0)
			return result;

		if (response == STM32F4_BOOTLOADER_ACK || response == STM32F4_BOOTLOADER_NACK) {

			/* Send ack is requested, make sure to clear the rx buf so we do not overwrite the response */
			value = STM32F4_BOOTLOADER_ACK;
			xfer.rx_buf = 0;
			result = spi_sync_transfer(mcu_spi->spi, &xfer, 1);
			if (result < 0)
				return result;

			/* Well we are done what ever the result */
			return response == STM32F4_BOOTLOADER_ACK ? 0 : -EIO;
		}

	} while (--retries > 0);

	/* We timed out */
	return -ETIMEDOUT;
}

static int stm32f4_sync_bootloader(struct mcu_spi *mcu_spi)
{
	int result;
	uint8_t sync = STM32F4_BOOTLOADER_SOF;
	struct spi_transfer xfer = { .tx_buf = &sync, .rx_buf = &sync, .len = sizeof(sync)};

	if (!mcu_spi || !mcu_spi->spi)
		return -EINVAL;

	/* Send the sync, fail on errors */
	result = spi_sync_transfer(mcu_spi->spi, &xfer, 1);
	if (result < 0)
		return -EIO;

	/* Did we get a sync response ? */
	if (sync != STM32F4_BOOTLOADER_SYNC)
		return -EIO;

	/* Wait for ack from the bootloader */
	result = stm32f4_wait_ack(mcu_spi);
	if (result < 0)
		return result;

	/* All good*/
	return 0;
}

static int stm32f4_send_cmd_frame(struct mcu_spi *mcu_spi, uint8_t cmd)
{
	int result;

	/* Build up the the command frame */
	uint8_t tx_cmd_frame[3] = { STM32F4_BOOTLOADER_SOF, cmd, ~cmd };
	uint8_t rx_cmd_frame[3] = { 0, 0, 0 };
	struct spi_transfer xfer = { .tx_buf = tx_cmd_frame, .rx_buf = rx_cmd_frame, .len = 3};

	/* Send the frame */
	result = spi_sync_transfer(mcu_spi->spi, &xfer, 1);
	if (result < 0)
		return result;

	/* Check for ack on the rx side */
	if (rx_cmd_frame[2] != STM32F4_BOOTLOADER_SYNC)
		dev_warn(&mcu_spi->spi->dev, "did not receive cmd frame sync (0x%02x)\n", rx_cmd_frame[2]);

	/* Wait for ack */
	result = stm32f4_wait_ack(mcu_spi);
	if (result < 0)
		return result;

	/* All good */
	return 0;
}

static int stm32f4_read_data_frame(struct mcu_spi *mcu_spi, void *buffer, size_t count)
{
	int result;
	uint8_t dummy = 0;
	struct spi_transfer xfers[2] = { { .tx_buf = &dummy, .len = sizeof(dummy)}, { .rx_buf = buffer, .len = count} };

	/* Read the data frame */
	result = spi_sync_transfer(mcu_spi->spi, xfers, ARRAY_SIZE(xfers));
	if (result < 0)
		return result;

	/* Return the count*/
	return count;
}

static int stm32f4_write_data_frame(struct mcu_spi *mcu_spi, void *buffer, size_t count)
{
	int result;
	uint8_t checksum = count > 1 ? stm32f4_xor_checksum(0, buffer, count) : ~*((uint8_t *)buffer);
	struct spi_transfer xfers[2] = { { .tx_buf = buffer, .len = count}, { .tx_buf = &checksum, .len = sizeof(checksum)} };

	/* Send it on it way */
	result = spi_sync_transfer(mcu_spi->spi, xfers, ARRAY_SIZE(xfers));
	if (result < 0)
		return result;

	/* Return the count */
	return count;
}

static int dev_node_match(struct device *dev, const void *data)
{
	return dev->of_node == data;
}

struct mcu_spi_device *mcu_spi_find(const char *name)
{
	int status = -ENOENT;
	struct device_node *spi_node;
	struct spi_device *spi;
	struct mcu_spi *mcu_spi;
	struct mcu_spi_device *pos = 0;
	struct mcu_spi_device *dev = 0;

	/* Try to find the device tree node */
	spi_node = of_find_node_by_name(NULL, "mcu-spi");
	if (!spi_node) {
		printk(KERN_ERR "could not find device node: 'mcu-spi'\n");
		return ERR_PTR(-ENOENT);
	}

	/* Find the spi device on the bus */
	spi = to_spi_device(bus_find_device(&spi_bus_type, NULL, spi_node, dev_node_match));
	if (!spi) {
		printk(KERN_ERR "could not find match 'mcu-spi' device");
		status = -ENODEV;
		goto error_release_spi_node;
	}

	/* Extract the driver data */
	mcu_spi = spi_get_drvdata(spi);

	/* Protect ourselves */
	mutex_lock(&mcu_spi->devices_lock);

	/* Search for the requested device */
	list_for_each_entry(pos, &mcu_spi->childern, device_node) {
		if (strcmp(name, pos->name) == 0) {
			dev = pos;
			break;
		}
	}

	/* Done with list */
	mutex_unlock(&mcu_spi->devices_lock);

	/* Did we find it? If so, up the reference count*/
 	if (dev) {
		status = 0;
		spi_dev_get(spi);
 	}

error_release_spi_node:
	of_node_put(spi_node);

	return status < 0 ? ERR_PTR(status) : dev;
}
EXPORT_SYMBOL(mcu_spi_find);

struct mcu_spi_device *mcu_spi_get(struct mcu_spi_device *mcu_spi_dev)
{
	/* Ignore if NULL */
	if (!mcu_spi_dev)
		return 0;

	/* Decrement reference count */
	spi_dev_get(mcu_spi_dev->parent->spi);

	return mcu_spi_dev;
}
EXPORT_SYMBOL(mcu_spi_get);

void mcu_spi_put(struct mcu_spi_device *mcu_spi_dev)
{
	/* Ignore if NULL */
	if (!mcu_spi_dev)
		return;

	/* Decrement reference count */
	spi_dev_put(mcu_spi_dev->parent->spi);
}
EXPORT_SYMBOL(mcu_spi_put);

int mcu_spi_lock(struct mcu_spi_device *mcu_spi_dev, unsigned int flags)
{
	struct mcu_spi *mcu_spi = mcu_spi_dev->parent;
	int status = 0;

	/* Save the flags */
	mcu_spi_dev->flags = flags;

	/* Blocking? */
	if (flags & O_NONBLOCK) {

		/* Exclusive? */
		if (flags & O_EXCL)
			status = down_write_trylock(&mcu_spi->mode_lock);
		else
			status = down_read_trylock(&mcu_spi->mode_lock);

	} else {

		/* Exclusive? */
		if (flags & O_EXCL) {
			status = down_write_killable(&mcu_spi->mode_lock);
			status = status == 0 ? 1 : status;
		}
		else {
			/* TODO need down read killable patch */
			down_read(&mcu_spi->mode_lock);
			status = 1;
		}
	}

	/* If successful up the reference count */
	if (status > 0)
		mcu_spi_get(mcu_spi_dev);

	return status;
}
EXPORT_SYMBOL(mcu_spi_lock);

void mcu_spi_unlock(struct mcu_spi_device *mcu_spi_dev)
{
	struct mcu_spi *mcu_spi = mcu_spi_dev->parent;

	/* Exclusive? */
	if (mcu_spi_dev->flags & O_EXCL)
		up_write(&mcu_spi->mode_lock);
	else
		up_read(&mcu_spi->mode_lock);

	/* Down the reference count */
	mcu_spi_put(mcu_spi_dev);
}
EXPORT_SYMBOL(mcu_spi_unlock);

int mcu_spi_chip_state(struct mcu_spi_device *mcu_spi_dev)
{
	return mcu_spi_dev->parent->chip_state;
}
EXPORT_SYMBOL(mcu_spi_chip_state);

static int mcu_spi_read_memory_unlocked(struct mcu_spi_device *mcu_spi_dev, uint32_t addr, void *buffer, size_t count)
{
	int status;
	uint8_t len = count - 1;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;
	uint32_t mcu_addr = cpu_to_be32(addr);

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state != MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Check the size must be less than 255 */
	if (count == 0 || count > 256)
		return -EINVAL;

	/* Send the read memory cmd */
	status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_READ_MEMORY);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send read memory cmd frame: %d\n", __func__, status);
		return status;
	}

	/* Send the read address */
	status = stm32f4_write_data_frame(mcu_spi, &mcu_addr, sizeof(addr));
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send read memory address 0x%08x: %d\n", __func__, addr, status);
		return status;
	}

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send read memory address 0x%08x: %d\n", __func__, addr, status);
		return status;
	}

	/* Send the read count */
	status = stm32f4_write_data_frame(mcu_spi, &len, sizeof(len));
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send read memory length %u: %d\n", __func__, len, status);
		return status;
	}

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send read memory address 0x%08x: %d\n", __func__, addr, status);
		return status;
	}

	/* Read the data */
	status = stm32f4_read_data_frame(mcu_spi, buffer, count);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to read data of length %u from 0x%08x: %d\n", __func__, len, addr, status);
		return status;
	}

	/* All good */
	return count;
}

static int mcu_spi_write_memory_unlocked(struct mcu_spi_device *mcu_spi_dev, uint32_t addr, const void *buffer, size_t count)
{
	int status;
	uint8_t len = (count - 1) & 0xff;
	uint8_t checksum = stm32f4_xor_checksum(len, buffer, count);
	struct spi_transfer xfers[3] = { { .tx_buf = &len, .len = sizeof(len)}, { .tx_buf = buffer, .len = count}, { .tx_buf = &checksum, .len = sizeof(checksum)} };
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state != MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Validate the buffer parameters */
	if (buffer == 0 || count > 256)
		return -EINVAL;

	/* Send the write memory cmd */
	status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_WRITE_MEMORY);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send write memory cmd frame: %d\n", __func__, status);
		return status;
	}

	/* Send the write address */
	addr = cpu_to_be32(addr);
	status = stm32f4_write_data_frame(mcu_spi, &addr, sizeof(addr));
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send write memory address (0x%08x): %d", __func__, addr, status);
		return status;
	}

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "%s - failed to send read memory address 0x%08x: %d\n", __func__, addr, status);
		return status;
	}

	/* Wait for the MCU at least 1msec */
	msleep(1);

	/* Send it on it way */
	status = spi_sync_transfer(mcu_spi->spi, xfers, ARRAY_SIZE(xfers));
	if (status < 0)
		return status;

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0)
		return status;

	/* All good */
	return count;
}

int mcu_spi_read_memory(struct mcu_spi_device *mcu_spi_dev, uint32_t addr, void *buffer, size_t count)
{
	int status;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Forward while locked */
	mutex_lock(&mcu_spi->function_lock);
	status = mcu_spi_read_memory_unlocked(mcu_spi_dev, addr, buffer, count);
	mutex_unlock(&mcu_spi->function_lock);

	/* Well..... */
	return status;
}
EXPORT_SYMBOL(mcu_spi_read_memory);

int mcu_spi_write_memory(struct mcu_spi_device *mcu_spi_dev, uint32_t addr, const void *buffer, size_t count)
{
	int status;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Forward while locked */
	mutex_lock(&mcu_spi->function_lock);
	status = mcu_spi_write_memory_unlocked(mcu_spi_dev, addr, buffer, count);
	mutex_unlock(&mcu_spi->function_lock);

	/* Well..... */
	return status;
}
EXPORT_SYMBOL(mcu_spi_write_memory);

static int mcu_spi_mass_erase_unlocked(struct mcu_spi_device *mcu_spi_dev)
{
	int status;
	uint8_t checksum;
	uint16_t mass_erase = cpu_to_be16(0xffff);
	struct spi_transfer xfers[2] = { { .tx_buf = &mass_erase, .len = sizeof(mass_erase)}, { .tx_buf = &checksum, .len = sizeof(checksum)} };
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	dev_info(&mcu_spi->spi->dev, "mass erasing mcu\n");

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state != MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Send the command frame */
	status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_ERASE);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed to send erase cmd frame: %d\n", status);
		return status;
	}

	/* Send the mass erase command */
	checksum = stm32f4_xor_checksum(0, &mass_erase, sizeof(mass_erase));
	status = spi_sync_transfer(mcu_spi->spi, xfers, ARRAY_SIZE(xfers));
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed to send mass erase frame: %d\n", status);
		return status;
	}

	/* Wait for the command to complete */
	msleep(STM32F411VE_MASS_ERASE_TIME);

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed mass erase: %d\n", status);
		return status;
	}

	dev_info(&mcu_spi->spi->dev, "mass erase completed\n");

	/* All good */
	return 0;
}

int mcu_spi_mass_erase(struct mcu_spi_device *mcu_spi_dev)
{
	int status;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Forward while locked */
	mutex_lock(&mcu_spi->function_lock);
	status = mcu_spi_mass_erase_unlocked(mcu_spi_dev);
	mutex_unlock(&mcu_spi->function_lock);

	/* Well..... */
	return status;
}
EXPORT_SYMBOL(mcu_spi_mass_erase);

int mcu_spi_reset(struct mcu_spi_device *mcu_spi_dev, int mode)
{
	int status = 0;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;
	uint8_t get_frame[13];
	uint8_t id_frame[3];
	uint8_t voltage_level = 0x03;
	uint32_t voltage_addr = 0xffff0000;

	/* Make sure we have a device */
	if (!mcu_spi_dev)
		return -EINVAL;

	/* Protect chip */
	mutex_lock(&mcu_spi->function_lock);

	/* Disable the mcu interrupts */
	disable_irq(mcu_spi->int_irq_num);

	/* Force the mcu into reset */
	gpiod_set_value(mcu_spi->gpio_mcu_reset, 0);

	/* Are we changing modes? */
	if (mode) {

		/* Adjust spi setup */
		if (mode == MCU_SPI_MODE_BOOTLOADER) {
			mcu_spi->spi->max_speed_hz = BOOTLOADER_SPI_SPEED_HZ;
			mcu_spi->spi->bits_per_word = BOOTLOADER_SPI_WIDTH;
			mcu_spi->spi->mode = BOOTLOADER_SPI_MODE;
		} else if (mode == MCU_SPI_MODE_FLASH) {
			mcu_spi->spi->max_speed_hz = FLASH_SPI_SPEED_HZ;
			mcu_spi->spi->bits_per_word = FLASH_SPI_WIDTH;
			mcu_spi->spi->mode = FLASH_SPI_MODE;
		}

		/* Reconfigure spi */
		status = spi_setup(mcu_spi->spi);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed setup spi device: %d\n", status);
			goto error_function_unlock;
		}

		/* Set boot pin state */
		gpiod_set_value(mcu_spi->gpio_mcu_boot, mode != MCU_SPI_MODE_BOOTLOADER);
	}

	/* Ensure the pulse is wide enough sleep at least 20 micro seconds per the STM32F411 datasheet */
	usleep_range(20, 100);

	/* Release the reset */
	gpiod_set_value(mcu_spi->gpio_mcu_reset, 1);

	/* Update the chip state */
	if (mode)
		mcu_spi->chip_state = mode;

	dev_info(&mcu_spi->spi->dev, "MCU mode now: %d\n", mcu_spi->chip_state);

	/* Are we launching the bootloader? */
	if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER) {

		/* Wait for the boot to be ready */
		usleep_range(200000, 400000);

		/* Sync to the bootloader */
		dev_info(&mcu_spi->spi->dev, "syncing with bootloader\n");
		status = stm32f4_sync_bootloader(mcu_spi);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed to sync with bootload: %d\n", status);
			goto error_mcu_reset;
		}

		/* Send the get cmd */
		status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_GET);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed to send get cmd frame: %d\n", status);
			goto error_mcu_reset;
		}

		/* Read the get data frame */
		status = stm32f4_read_data_frame(mcu_spi, get_frame, ARRAY_SIZE(get_frame));
		if (status < 0 || get_frame[0] != 11) {
			status = status < 0 ? status : -EIO;
			dev_warn(&mcu_spi->spi->dev, "problem reading version and capabilities: 0x%02x, %d\n", get_frame[0], status);
			goto error_mcu_reset;
		}

		/* Wait for ack */
		status = stm32f4_wait_ack(mcu_spi);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed mass erase: %d\n", status);
			goto error_mcu_reset;
		}

		/* Send the get chip id cmd */
		status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_GET_ID);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed to send get id cmd frame: %d\n", status);
			goto error_mcu_reset;
		}

		/* Read the chip id data frame */
		status = stm32f4_read_data_frame(mcu_spi, id_frame, ARRAY_SIZE(id_frame));
		if (status < 0 || id_frame[0] != 1) {
			status = status < 0 ? status : -EIO;
			dev_warn(&mcu_spi->spi->dev, "problem reading chip id: 0x%02x, %d\n", id_frame[0], status);
			goto error_mcu_reset;
		}

		/* Wait for ack */
		status = stm32f4_wait_ack(mcu_spi);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed mass erase: %d\n", status);
			goto error_mcu_reset;
		}

		/* Log device id */
		dev_info(&mcu_spi->spi->dev, "found device id: %02x%02x with bootloader v%u.%u\n", id_frame[1], id_frame[2], (get_frame[1] >> 4), (get_frame[1] & 0x0f));

		/* Set the voltage to 3.3V */
		status = mcu_spi_write_memory_unlocked(mcu_spi_dev, voltage_addr, &voltage_level, sizeof(voltage_level));
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "problem setting MCU voltage level: %d\n", status);
			goto error_mcu_reset;
		}
	} else if (mcu_spi->chip_state == MCU_SPI_MODE_FLASH) {

		/* Wait for the mcu to become ready */
		status = mcu_spi_wait_for_state(mcu_spi->spi, true);
		if (status < 0)
			goto error_function_unlock;

		/* Enable the IRQ */
		enable_irq(mcu_spi->int_irq_num);
	}

	/* All done */
	mutex_unlock(&mcu_spi->function_lock);
	return 0;

error_mcu_reset:
	gpiod_set_value(mcu_spi->gpio_mcu_reset, 0);
	usleep_range(20, 100);
	gpiod_set_value(mcu_spi->gpio_mcu_reset, 1);
	mcu_spi->chip_state = MCU_SPI_MODE_FLASH;
	enable_irq(mcu_spi->int_irq_num);

error_function_unlock:
	mutex_unlock(&mcu_spi->function_lock);

	return status;

}
EXPORT_SYMBOL(mcu_spi_reset);

int mcu_spi_setup(struct mcu_spi_device *mcu_spi_dev, unsigned int speed, unsigned char width, unsigned short mode)
{
	int status;
	uint32_t old_max_speed_hz;
	uint8_t	old_bits_per_word;
	uint16_t old_mode;

	if (!mcu_spi_dev || !mcu_spi_dev->parent)
		return -EINVAL;

	/* Save to restore on failure */
	old_max_speed_hz = mcu_spi_dev->parent->spi->max_speed_hz;
	old_bits_per_word = mcu_spi_dev->parent->spi->bits_per_word;
	old_mode = mcu_spi_dev->parent->spi->mode;

	/* Update */
	mcu_spi_dev->parent->spi->max_speed_hz = speed;
	mcu_spi_dev->parent->spi->bits_per_word = width;
	mcu_spi_dev->parent->spi->mode = mode;

	/* Change the setup */
	status = spi_setup(mcu_spi_dev->parent->spi);
	if (status < 0) {
		dev_warn(&mcu_spi_dev->parent->spi->dev, "failed setup spi device: %d\n", status);
		mcu_spi_dev->parent->spi->max_speed_hz = old_max_speed_hz;
		mcu_spi_dev->parent->spi->bits_per_word = old_bits_per_word;
		mcu_spi_dev->parent->spi->mode = old_mode;
		return status;
	}

	/* All good */
	return 0;
}
EXPORT_SYMBOL(mcu_spi_setup);

int mcu_spi_load_image(struct mcu_spi_device *mcu_spi_dev, const char *name, int flags)
{
	const struct firmware *firmware;
	uint32_t load_addr;
	int status = 0;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;
	ssize_t write_size;
	ssize_t written;
	ssize_t amount = 0;

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state != MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Protect chip */
	mutex_lock(&mcu_spi->function_lock);

	/* Request the firmware blob */
	status = request_firmware_direct(&firmware, name, &mcu_spi->spi->dev);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "problem loading firmware image '%s': %d\n", name, status);
		goto error_function_unlock;
	}

	/* Extract the pieces, the load address is in an unused cortex-m4 vector and the launch address is reset vector */
	load_addr = *(((uint32_t *)firmware->data) + 8) == 0 ? STM32F411VE_FLASH_START : *(((uint32_t *)firmware->data) + 8);

	/* Do we need to erase the chip? */
	if (load_addr == STM32F411VE_FLASH_START) {
		status = mcu_spi_mass_erase_unlocked(mcu_spi_dev);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed to erase mcu: %d\n", status);
			goto error_release_firmware;
		}
	}

	dev_info(&mcu_spi->spi->dev, "loading '%s' to 0x%08x with size %u\n", name, load_addr, firmware->size);

	/* Write the image */
	amount = 0;
	while (amount < firmware->size) {

		/* Limit to max of flash block size */
		write_size = firmware->size - amount < STM32F4_BOOTLOADER_BLOCK_SIZE ? firmware->size - amount : STM32F4_BOOTLOADER_BLOCK_SIZE;

		/* Write the block */
		written = mcu_spi_write_memory_unlocked(mcu_spi_dev, load_addr + amount, firmware->data + amount, write_size);
		if (written < 0) {
			dev_warn(&mcu_spi->spi->dev, "problem writing block of size %d to 0x%08x: %d\n", write_size, load_addr + amount, written);
			goto error_release_firmware;
		}

		/* Update the amount written */
		amount += written;
	}

	/* Should we verify write? */
	if (flags & MCU_SPI_BL_LOAD_VERIFY)
		dev_info(&mcu_spi->spi->dev, "verify not implemented\n");

	/* Trailing actions */
	if (flags & MCU_SPI_BL_LOAD_LAUNCH) {

		dev_info(&mcu_spi->spi->dev, "launching '%s' @ 0x%08x\n", name, load_addr);

		/* Send the go command frame */
		status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_GO);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed to send go cmd frame: %d\n", status);
			goto error_release_firmware;
		}

		/* Send the go address */
		load_addr = cpu_to_be32(load_addr);
		status = stm32f4_write_data_frame(mcu_spi, &load_addr, sizeof(load_addr));
		if (status < 0)
			dev_warn(&mcu_spi->spi->dev, "failed to send go address 0x%08x: %d", be32_to_cpu(load_addr), status);

		/* Wait for ack */
		status = stm32f4_wait_ack(mcu_spi);
		if (status < 0) {
			dev_warn(&mcu_spi->spi->dev, "failed wait for address ack: %d\n", status);
			goto error_release_firmware;
		}

		/* Update state */
		mcu_spi->chip_state = MCU_SPI_MODE_SRAM;

	} else if (flags & MCU_SPI_BL_LOAD_FLASH) {

		dev_info(&mcu_spi->spi->dev, "resetting: '%s'\n", name);

		/* Reset the mcu into flash mode */
		status = mcu_spi_reset(mcu_spi_dev, MCU_SPI_MODE_FLASH);
		if (status < 0)
			dev_warn(&mcu_spi->spi->dev, "problem resetting MCU flash mode '%s': %d", name, status);

		/* Update state */
		mcu_spi->chip_state = MCU_SPI_MODE_FLASH;
	}

	dev_info(&mcu_spi->spi->dev, "MCU mode now: %d\n", mcu_spi_chip_state(mcu_spi_dev));

error_release_firmware:

	/* Release the firmware */
	release_firmware(firmware);

error_function_unlock:
	mutex_unlock(&mcu_spi->function_lock);

	/* Return error */
	return status > 0 ? 0 : status;
}
EXPORT_SYMBOL(mcu_spi_load_image);

static int mcu_spi_erase_pages_unlocked(struct mcu_spi_device *mcu_spi_dev, int *pages, size_t count)
{
	int status;
	uint16_t len;
	int i;
	int est_time = 0;
	uint16_t page_set[STM32F411VE_NUM_PAGES];
	uint8_t checksum;
	struct spi_transfer xfers[2] = { { .tx_buf = page_set, .len = count * 2}, { .tx_buf = &checksum, .len = sizeof(checksum)} };
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state != MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Check the number of pages */
	if (pages == 0 || count > 8)
		return -EINVAL;

	dev_info(&mcu_spi->spi->dev, "erasing %d pages\n", count);

	/* Build page set, time estimate base on STM32F4V11E datasheets */
	for (i = 0; i < count; ++i) {
		if (pages[i] < 0 || pages[i] > STM32F411VE_NUM_PAGES - 1)
			return -EINVAL;
		est_time += page_erase_times[pages[i]];
		page_set[i] = cpu_to_be16(pages[i]);
	}

	/* Send the command frame */
	status = stm32f4_send_cmd_frame(mcu_spi, STM32F4_BOOTLOADER_ERASE);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed to send erase cmd frame: %d\n", status);
		return status;
	}

	/* Send the number of pages */
	len = cpu_to_be16((count - 1) & 0xffff);
	status = stm32f4_write_data_frame(mcu_spi, &len, sizeof(len));
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed to send erase page count frame: %d\n", status);
		return status;
	}

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed mass erase: %d\n", status);
		return status;
	}

	/* Send the page set */
	checksum = stm32f4_xor_checksum(0, page_set, count * 2);
	status = spi_sync_transfer(mcu_spi->spi, xfers, ARRAY_SIZE(xfers));
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed to send erase page set frame: %d\n", status);
		return status;
	}

	/* Wait for the command to complete */
	msleep(est_time);

	/* Wait for ack */
	status = stm32f4_wait_ack(mcu_spi);
	if (status < 0) {
		dev_warn(&mcu_spi->spi->dev, "failed erase page set after waiting %d: %d\n", est_time, status);
		return status;
	}

	dev_info(&mcu_spi->spi->dev, "done erasing %d pages\n", count);

	/* All good */
	return 0;
}

int mcu_spi_erase_pages(struct mcu_spi_device *mcu_spi_dev, int *pages, size_t count)
{
	int status;
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Forward while locked */
	mutex_lock(&mcu_spi->function_lock);
	status = mcu_spi_erase_pages_unlocked(mcu_spi_dev, pages, count);
	mutex_unlock(&mcu_spi->function_lock);

	/* Well..... */
	return status;
}
EXPORT_SYMBOL(mcu_spi_erase_pages);

int mcu_spi_pump(struct mcu_spi_device *mcu_spi_dev, const void *tx, void *rx, size_t count)
{
	int status;
	int retries = 5;
	struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx, .len = count };
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	if (!count) {
		dev_warn(&mcu_spi->spi->dev, "tried to pump zero bytes\n");
		return -EINVAL;
	}

	++mcu_spi->pump_cnt;

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Try a couple of time to workaround unknown spi master bug */
	do {
		status = spi_sync_transfer(mcu_spi->spi, &xfer, 1);
		if (status >= 0)
			break;
		dev_warn(&mcu_spi->spi->dev, "%llu pump(%d) failed: %d\n", mcu_spi->pump_cnt, retries, status);
	} while (--retries > 0);

	/* Return the last status */
	if (status != 0)
		dev_warn(&mcu_spi->spi->dev, "%llu pump(%d) returning: %d\n", mcu_spi->pump_cnt, retries, status);

	/* Hopefully very thing is good */
	return status;
}
EXPORT_SYMBOL(mcu_spi_pump);

void mcu_spi_trans_init(struct mcu_spi_transaction *trans, void *cmd, size_t csize, void *payload, size_t psize, void (*done)(struct mcu_spi_transaction *trans, size_t count, int status))
{
	struct spi_delay delay = { .value = 0, .unit = SPI_DELAY_UNIT_USECS };
	trans->cmd = cmd;
	trans->size = csize + psize;
	trans->payload = payload;
	trans->done = done;
	trans->context = 0;

	memset(trans->xfers, 0, sizeof(trans->xfers));

	trans->xfers[0].tx_buf = cmd;
	trans->xfers[0].rx_buf = cmd;
	trans->xfers[0].len = csize;
	trans->xfers[0].cs_change = 1;
	trans->xfers[0].delay = delay;

	trans->xfers[1].tx_buf = payload;
	trans->xfers[1].rx_buf = payload;
	trans->xfers[1].len = psize;
	trans->xfers[1].cs_change = 0;
	trans->xfers[1].delay = delay;

	spi_message_init_with_transfers(&trans->msg, trans->xfers, ARRAY_SIZE(trans->xfers));
}
EXPORT_SYMBOL(mcu_spi_trans_init);

static void mcu_spi_sync_complete(void *data)
{
	struct completion *completion = data;

	/* Release the waiter */
	complete(completion);
}

static void mcu_spi_async_complete(void *data)
{
	struct mcu_spi_transaction *trans = data;
	ktime_get_ts64(&trans->end);
	if (trans->done)
		trans->done(trans, trans->msg.actual_length, trans->msg.status);
}

int mcu_spi_async(struct mcu_spi_device *mcu_spi_dev, struct mcu_spi_transaction *trans)
{
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Check the state and run boot loader if necessary */
	if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Bind the forwarding completion handler */
	trans->msg.complete = mcu_spi_async_complete;
	trans->msg.context = trans;
	ktime_get_ts64(&trans->start);

	/* Return the async status */
	return spi_async(mcu_spi->spi, &trans->msg);
}
EXPORT_SYMBOL(mcu_spi_async);

int mcu_spi_sync(struct mcu_spi_device *mcu_spi_dev, void *cmd, size_t csize, void *payload, size_t psize)
{
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;
	int status;
	struct mcu_spi_transaction trans;
	DECLARE_COMPLETION_ONSTACK(msg_complete);

	/* Check the state */
	if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER)
		return -EBUSY;

	/* Initialize the transaction */
	mcu_spi_trans_init(&trans, cmd, csize, payload, psize, 0);
	trans.msg.complete = mcu_spi_sync_complete;
	trans.msg.context = &msg_complete;

	/* Send the message on */
	status = spi_async(mcu_spi->spi, &trans.msg);
	if (status < 0) {
		dev_warn(mcu_spi_dev->this, "mcu_spi_sync: failed to launch async transaction: %d\n", status);
		return status;
	}

	/* Wait for the message and checkout completion status */
	status = wait_for_completion_timeout(&msg_complete, 5 * HZ);
	if (status < 0)
		panic("spi transaction timed out after %d jiffies", 5 * HZ);

	/* All done */
	return trans.msg.status < 0 ? trans.msg.status : psize;
}
EXPORT_SYMBOL(mcu_spi_sync);

int mcu_spi_send_reset(struct mcu_spi_device *mcu_spi_dev)
{
	int status = 0;
	int max_timeout = 250;
	int retry = 10;
	struct mcu_spi *mcu_spi = mcu_spi_dev->parent;

	/* Check the state */
	if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER) {
		dev_warn(mcu_spi_dev->this, "mcu is in bootloader mode bailing\n");
		return -EBUSY;
	}

	/* First we need to own the bus */
	spi_bus_lock(mcu_spi->spi->controller);

	/* attept cs wiggle up to 10 times */
	while (retry-- > 0) {
		/* Wrap the cs line calls in a critical section to prevent premption from messing with the timing of the cs wiggle */
		local_irq_disable();

		/* force the CS low */
		mcu_spi->spi->controller->set_cs(mcu_spi->spi, true);

		/* Busy wait for the mcu to go busy */
		/* mcu_ready line needs to go low in less than 10us or else the mcu will miss the reset signal */
		max_timeout = 250;
		while (--max_timeout > 0 && gpiod_get_value(mcu_spi->gpio_mcu_ready) == 1);

		/* Release the CS */
		mcu_spi->spi->controller->set_cs(mcu_spi->spi, false);

		/* Exit the critical section */
		local_irq_enable();

		/* Wait for the mcu to be ready again if we did not timeout */
		if (max_timeout > 0) {
			status = mcu_spi_wait_for_state(mcu_spi_dev->parent->spi, true);
			if (status == 0) break;
		} else {
			status = -ETIMEDOUT;
		}
	}

	/* Unlock the bus */
	spi_bus_unlock(mcu_spi->spi->controller);

	dev_dbg(&mcu_spi->spi->dev, "mcu_spi_send_reset: %d %d\n", max_timeout, status);

	/* Return any timeout */
	return status;
}
EXPORT_SYMBOL(mcu_spi_send_reset);

static irqreturn_t mcu_int_irq_handler(int irq, void *data)
{
	struct mcu_spi_event *entry;
	struct mcu_spi *mcu_spi = data;
	int status;

	while (gpiod_get_value(mcu_spi->gpio_mcu_int))
	{
		dev_dbg(&mcu_spi->spi->dev, "handling mcu int\n");

		/* Check for bad state */
		if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER) {
			dev_warn(&mcu_spi->spi->dev, "bad chip state disabling mcu int irq: %d\n", mcu_spi->chip_state);
			disable_irq_nosync(mcu_spi->int_irq_num);
			break;
		}

		/* Protect the list, this is ok because we are in a threaded irq */
		mutex_lock(&mcu_spi->event_lock);

		list_for_each_entry(entry, &mcu_spi->event_handlers, node) {
			status = entry->notify(MCU_SPI_INT_EVENT, entry->context);
			if (status < 0) {
				dev_err(&mcu_spi->spi->dev, "handling spi interrupt, disabling: %d\n", status);
				disable_irq_nosync(mcu_spi->int_irq_num);
				break;
			}
		}

		/* Good to go */
		mutex_unlock(&mcu_spi->event_lock);
	}

	return IRQ_HANDLED;
}

static irqreturn_t mcu_ready_irq_handler(int irq, void *data)
{
	struct mcu_spi *mcu_spi = data;

	/* Kick any waiters */
	wake_up_all(&mcu_spi->mcu_ready_wq);

	return IRQ_HANDLED;
}

static int mcu_spi_wait_for_state(struct spi_device *spi, bool ready)
{
	int status;
	struct mcu_spi *mcu_spi = spi_get_drvdata(spi);

	/* MCU is always ready in bootloader mode */
	if (mcu_spi->chip_state == MCU_SPI_MODE_BOOTLOADER)
		return 0;

	/* Wait for ready? */
	status = wait_event_timeout(mcu_spi->mcu_ready_wq, gpiod_get_value(mcu_spi->gpio_mcu_ready) == (ready? 1:0), HZ);
	if (status <= 0) {
		dev_warn(&spi->dev, "problem waiting for mcu to become %s: %d %d\n", ready? "ready":"busy", status, gpiod_get_value(mcu_spi->gpio_mcu_ready));
		if (status == 0)
			return -ETIMEDOUT;
	}

	return 0;
}

void mcu_spi_register_event(struct mcu_spi_device *mcu_spi_dev, struct mcu_spi_event *event)
{
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;
	int status;

	/* Protect the list */
	mutex_lock(&mcu_spi->event_lock);

	/* Add to the list of event handlers */
	list_add(&event->node, &mcu_spi->event_handlers);

	/* Good to go */
	mutex_unlock(&mcu_spi->event_lock);

	/* Bind mcu-int interrupt handler if this is the first event handler */
	if (list_is_singular(&mcu_spi->event_handlers)) {
		status = devm_request_threaded_irq(&mcu_spi->spi->dev, mcu_spi->int_irq_num, 0, mcu_int_irq_handler, IRQF_TRIGGER_LOW | IRQF_ONESHOT, "MCU-INT", mcu_spi);
		if (status < 0)
			dev_err(&mcu_spi->spi->dev, "failed to request mcu-int threaded interrupt: %d\n", status);
	}

	dev_dbg(&mcu_spi->spi->dev, "add event handler %p with callback %p:%p\n", event, event->notify, event->context);

/*	mcu_int_irq_handler(mcu_spi_dev->parent->int_irq_num, mcu_spi_dev->parent);*/
}
EXPORT_SYMBOL(mcu_spi_register_event);

void mcu_spi_unregister_event(struct mcu_spi_device *mcu_spi_dev, struct mcu_spi_event *event)
{
	struct mcu_spi * mcu_spi = mcu_spi_dev->parent;

	/* Protect the list */
	mutex_lock(&mcu_spi->event_lock);

	if (list_is_singular(&mcu_spi->event_handlers))
		devm_free_irq(&mcu_spi->spi->dev, mcu_spi->int_irq_num, mcu_spi);

	/* Add to the list of event handlers */
	list_del(&event->node);
	INIT_LIST_HEAD(&event->node);

	/* Good to go */
	mutex_unlock(&mcu_spi->event_lock);

	dev_dbg(&mcu_spi->spi->dev, "removed event handler %p with callback %p:%p\n", event, event->notify, event->context);
}
EXPORT_SYMBOL(mcu_spi_unregister_event);


static int mcu_open(struct inode *inode, struct file *file)
{
	int status = -ENODEV;
	struct device_node *spi_node;
	struct spi_device *spi;
	struct mcu_spi *mcu_spi;
	struct mcu_spi_device *mcu_spi_dev;
	const struct file_operations *new_fops = NULL;
	int minor = iminor(inode);

	/* Try to find the device tree node */
	spi_node = of_find_node_by_name(NULL, "mcu-spi");
	if (!spi_node) {
		printk(KERN_ERR "could not find device node: 'mcu-spi'\n");
		return -ENOENT;
	}

	/* Find the spi device on the bus */
	spi = to_spi_device(bus_find_device(&spi_bus_type, NULL, spi_node, dev_node_match));
	if (!spi) {
		printk(KERN_ERR "could not find match 'mcu-spi' device");
		goto error_release_spi_node;
	}

	/* Extract the driver data */
	mcu_spi = spi_get_drvdata(spi);

	/* Protect ourselves */
	mutex_lock(&mcu_spi->devices_lock);

	/* Search for match minor number, fail if not found */
	list_for_each_entry(mcu_spi_dev, &mcu_spi->childern, device_node) {
		if (MINOR(mcu_spi_dev->devnode) == minor) {
			new_fops = fops_get(mcu_spi_dev->ops);
			break;
		}
	}
	if (!new_fops)
		goto error_unlock_list;

	/* Hide the pointer the file private data */
	file->private_data = mcu_spi_dev;

	/* Forward to the device */
	replace_fops(file, new_fops);
	if (file->f_op->open)
		status = file->f_op->open(inode,file);
	else
		status = -ENOTSUPP;

error_unlock_list:
	mutex_unlock(&mcu_spi->devices_lock);

error_release_spi_node:
	of_node_put(spi_node);

	/* Maybe it worked */
	return status;
}

int mcu_spi_add_attr(struct mcu_spi_device *mcu_spi_dev, const char *name, mcu_spi_dev_attr_show_t show, mcu_spi_dev_attr_store_t store, void *context)
{
	int status;

	/* Allocate a new attribute */
	struct mcu_spi_device_attribute *dev_attr = kzalloc(sizeof(struct mcu_spi_device_attribute), GFP_KERNEL);
	if (!dev_attr)
		return -ENOMEM;

	/* Initialize node list and context */
	INIT_LIST_HEAD(&dev_attr->node);
	dev_attr->context = context;

	/* Bind the name */
	dev_attr->parent.attr.name = kstrdup(name, GFP_KERNEL);
	if (!dev_attr->parent.attr.name) {
		status = -ENOMEM;
		goto error_free_attribute;
	}

	/* Build up the mode based on the callback provided */
	if (show)
		dev_attr->parent.attr.mode |= S_IRUGO;
	if (store)
		dev_attr->parent.attr.mode |= S_IWUGO;

	/* Save the call backs */
	dev_attr->parent.show = show;
	dev_attr->parent.store = store;

	/* Create the file */
	status = device_create_file(mcu_spi_dev->this, &dev_attr->parent);
	if (status != 0)
		goto error_free_name;

	/* Add the attribute list */
	mutex_lock(&mcu_spi_dev->lock);
	list_add(&dev_attr->node, &mcu_spi_dev->attributes);
	mutex_unlock(&mcu_spi_dev->lock);

	/* All done */
	return 0;

error_free_name:
	kfree(dev_attr->parent.attr.name);

error_free_attribute:
	kfree(dev_attr);

	return status;
}
EXPORT_SYMBOL(mcu_spi_add_attr);


int mcu_spi_remove_attr(struct mcu_spi_device *mcu_spi_dev, const char *name)
{
	struct mcu_spi_device_attribute *dev_attr = 0;

	mutex_lock(&mcu_spi_dev->lock);
	list_for_each_entry(dev_attr, &mcu_spi_dev->attributes, node) {
		if (strcmp(name, dev_attr->parent.attr.name) == 0) {
			list_del(&dev_attr->node);
			break;
		}
	}
	mutex_unlock(&mcu_spi_dev->lock);

	/* Did we find it? */
	if (&dev_attr->node == &mcu_spi_dev->attributes)
		return -ENOENT;

	/* Remove the attribute file */
	device_remove_file(mcu_spi_dev->this, &dev_attr->parent);

	/* Release the name */
	kfree(dev_attr->parent.attr.name);

	/* Release the attribute */
	kfree(dev_attr);

	return 0;
}

static const struct file_operations mcu_fops = {
	.owner		= THIS_MODULE,
	.open		= mcu_open,
	.llseek		= noop_llseek,
};
EXPORT_SYMBOL(mcu_spi_remove_attr);

static char *mcu_devnode(const struct device *dev, umode_t *mode)
{
	struct mcu_spi_device *mcu_spi_dev = dev_get_drvdata(dev);
	char node_name[64];

	if (mode && mcu_spi_dev->mode)
		*mode = mcu_spi_dev->mode;

	/* Build up the node name */
	sprintf(node_name, "mcu/%s", mcu_spi_dev->name);

	return kstrdup(node_name, GFP_KERNEL);
}

int mcu_spi_register(struct mcu_spi_device *mcu_spi_dev)
{
	int status = 0;
	struct mcu_spi *mcu_spi;
	struct device_node *spi_node;
	struct spi_device *spi_dev;

	/* Try to find the device tree node */
	spi_node = of_find_node_by_name(NULL, "mcu-spi");
	if (!spi_node) {
		printk(KERN_ERR "could not find device node: 'mcu-spi'\n");
		return -ENOENT;
	}

	/* Find the spi device on the bus */
	spi_dev = to_spi_device(bus_find_device(&spi_bus_type, NULL, spi_node, dev_node_match));
	if (!spi_dev) {
		printk(KERN_ERR "could not find match 'mcu-spi' device");
		status = -ENODEV;
		goto error_release_spi_node;
	}

	/* Extract the driver data */
	mcu_spi = spi_get_drvdata(spi_dev);

	/* Initialize the lists */
	INIT_LIST_HEAD(&mcu_spi_dev->device_node);
	INIT_LIST_HEAD(&mcu_spi_dev->event_node);
	INIT_LIST_HEAD(&mcu_spi_dev->attributes);

	/* Initialize the lock */
	mutex_init(&mcu_spi_dev->lock);

	/* Up the refcount and initialize the parent to the mcu_spi */
	mcu_spi_dev->parent = mcu_spi;
	mcu_spi_get(mcu_spi_dev);

	/* Protect ourselves */
	mutex_lock(&mcu_spi->devices_lock);

	/* Allocate a device number */
	mcu_spi_dev->devnode = MKDEV(mcu_spi->mcu_major, ++mcu_spi->mcu_minor);

	/* Create an matching device */
	mcu_spi_dev->this =	device_create_with_groups(mcu_spi->mcu_class, &mcu_spi_dev->parent->spi->dev, mcu_spi_dev->devnode, mcu_spi_dev, mcu_spi_dev->groups, "%s", mcu_spi_dev->name);
	if (IS_ERR(mcu_spi_dev->this)) {

		/* Clean up */
		mcu_spi_put(mcu_spi_dev);
		--mcu_spi->mcu_minor;

		/* Log */
		status = PTR_ERR(mcu_spi_dev->this);
		printk(KERN_ERR "could not create matching device: %d\n", status);

		/* Escape */
		goto error_unlock;
	}

	/* Add it to the list */
	list_add(&mcu_spi_dev->device_node, &mcu_spi->childern);

error_unlock:

	/* All done */
	mutex_unlock(&mcu_spi->devices_lock);

error_release_spi_node:

	/* Release the spi node */
	of_node_put(spi_node);

	/* All done */
	return status;
}
EXPORT_SYMBOL(mcu_spi_register);

void mcu_spi_unregister(struct mcu_spi_device *mcu_spi_dev)
{
	struct mcu_spi_device_attribute *cur_attr;
	struct mcu_spi_device_attribute *next_attr;

	/* Protect ourselves */
	mutex_lock(&mcu_spi_dev->parent->devices_lock);

	/* Remove ourselves from the device list */
	list_del(&mcu_spi_dev->device_node);

	/* Destroy any attributes remaining */
	list_for_each_entry_safe(cur_attr, next_attr, &mcu_spi_dev->attributes, node)
	{
		/* Remove from the list */
		list_del(&cur_attr->node);

		/* Unbind the attribute */
		device_remove_file(mcu_spi_dev->this, &cur_attr->parent);

		/* Release the name */
		kfree(cur_attr->parent.attr.name);

		/* Release the attribute */
		kfree(cur_attr);
	}

	/* Destroy the matching device */
	device_destroy(mcu_spi_dev->parent->mcu_class, mcu_spi_dev->devnode);

	/* Release the spi device */
	mcu_spi_put(mcu_spi_dev);

	/* All done */
	mutex_unlock(&mcu_spi_dev->parent->devices_lock);
}
EXPORT_SYMBOL(mcu_spi_unregister);

static ssize_t show_board_id(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mcu_spi *mcu_spi_dev = dev_get_drvdata(dev);
	unsigned int idx;
	unsigned int board_id = 0;

	for (idx = 0; idx < mcu_spi_dev->gpio_mcu_board_id->ndescs; ++idx)
		board_id |= gpiod_get_value_cansleep(mcu_spi_dev->gpio_mcu_board_id->desc[idx]) << idx;

	return scnprintf(buf, PAGE_SIZE, "%u\n", board_id);
}
static DEVICE_ATTR(board_id, S_IRUGO, show_board_id, 0);

static int mcu_spi_probe(struct spi_device *spi)
{
	int status;
	struct mcu_spi *mcu_spi;

	/* Allocate the driver data */
	mcu_spi = devm_kzalloc(&spi->dev, sizeof(struct mcu_spi), GFP_KERNEL);
	if (!mcu_spi) {
		dev_err(&spi->dev, "failed to allocate mcu spi driver\n");
		return -ENOMEM;
	}

	/* Initialize the locks */
	mutex_init(&mcu_spi->devices_lock);
	mutex_init(&mcu_spi->function_lock);
	init_rwsem(&mcu_spi->mode_lock);
	mutex_init(&mcu_spi->event_lock);

	/* Initialize notifications list */
	INIT_LIST_HEAD(&mcu_spi->event_handlers);
	INIT_LIST_HEAD(&mcu_spi->childern);

	/* Set up the gpio descriptors */
	mcu_spi->gpio_mcu_reset = devm_gpiod_get(&spi->dev, "mcu-reset", GPIOD_ASIS);
	if (IS_ERR_VALUE(mcu_spi->gpio_mcu_reset)) {
		dev_err(&spi->dev, "problem getting reset gpio: %ld", PTR_ERR(mcu_spi->gpio_mcu_reset));
		return PTR_ERR(mcu_spi->gpio_mcu_reset);
	}

	mcu_spi->gpio_mcu_boot = devm_gpiod_get(&spi->dev, "mcu-boot", GPIOD_ASIS);
	if (IS_ERR_VALUE(mcu_spi->gpio_mcu_boot)) {
		dev_err(&spi->dev, "problem getting boot gpio: %ld", PTR_ERR(mcu_spi->gpio_mcu_boot));
		return PTR_ERR(mcu_spi->gpio_mcu_boot);
	}

	mcu_spi->gpio_mcu_int = devm_gpiod_get(&spi->dev, "mcu-int", GPIOD_ASIS);
	if (IS_ERR_VALUE(mcu_spi->gpio_mcu_int)) {
		dev_err(&spi->dev, "problem getting interrupt gpio: %ld", PTR_ERR(mcu_spi->gpio_mcu_int));
		return PTR_ERR(mcu_spi->gpio_mcu_int);
	}

	mcu_spi->gpio_mcu_ready = devm_gpiod_get(&spi->dev, "mcu-ready", GPIOD_ASIS);
	if (IS_ERR_VALUE(mcu_spi->gpio_mcu_ready)) {
		dev_err(&spi->dev, "problem getting ready gpio: %ld", PTR_ERR(mcu_spi->gpio_mcu_ready));
		return PTR_ERR(mcu_spi->gpio_mcu_ready);
	}

	mcu_spi->gpio_mcu_board_id = devm_gpiod_get_array(&spi->dev, "mcu-board-id", GPIOD_ASIS);
	if (IS_ERR_VALUE(mcu_spi->gpio_mcu_board_id)) {
		dev_err(&spi->dev, "problem getting board id gpios: %ld", PTR_ERR(mcu_spi->gpio_mcu_board_id));
		return PTR_ERR(mcu_spi->gpio_mcu_board_id);
	}

	/* Retrieve the irq number for the interrupt pin */
	mcu_spi->int_irq_num = gpiod_to_irq(mcu_spi->gpio_mcu_int);
	if (mcu_spi->int_irq_num < 0) {
		dev_err(&spi->dev, "problem mapping mcu-int to irq number: %d", mcu_spi->int_irq_num);
		return mcu_spi->int_irq_num;
	}

	/* Retrieve the irq number for the ready pin */
	mcu_spi->ready_irq_num = gpiod_to_irq(mcu_spi->gpio_mcu_ready);
	if (mcu_spi->ready_irq_num < 0) {
		dev_err(&spi->dev, "problem mapping mcu-ready to irq number: %d", mcu_spi->ready_irq_num);
		return mcu_spi->ready_irq_num;
	}

	/* Initialize the state */
	mcu_spi->chip_state = gpiod_get_value(mcu_spi->gpio_mcu_boot) ? MCU_SPI_MODE_BOOTLOADER : MCU_SPI_MODE_FLASH;

	/* Bind driver */
	mcu_spi->spi = spi;
	spi_set_drvdata(spi, mcu_spi);

	/* Initialize the device ready wait queue */
	init_waitqueue_head(&mcu_spi->mcu_ready_wq);

	/* Adjust spi configuration */
	if (mcu_spi->chip_state == MCU_SPI_MODE_FLASH) {
		mcu_spi->spi->max_speed_hz = FLASH_SPI_SPEED_HZ;
		mcu_spi->spi->bits_per_word = FLASH_SPI_WIDTH;
		mcu_spi->spi->mode = FLASH_SPI_MODE;
	} else {
		mcu_spi->spi->max_speed_hz = BOOTLOADER_SPI_SPEED_HZ;
		mcu_spi->spi->bits_per_word = BOOTLOADER_SPI_WIDTH;
		mcu_spi->spi->mode = BOOTLOADER_SPI_MODE;
	}

	/* Attache the ready line to the master */
	spi_imx_hook_device_wait_for_state(spi->controller, DEVICE_READY);

	/* Reconfigure spi */
	status = spi_setup(mcu_spi->spi);
	if (status < 0) {
		dev_err(&mcu_spi->spi->dev, "failed setup spi device: %d\n", status);
		return status;
	}

	/* Create the mcu class */
	mcu_spi->mcu_class = class_create("mcu");
	if (IS_ERR(mcu_spi->mcu_class)) {
		dev_err(&mcu_spi->spi->dev, "failed to create mcu class: %ld\n", PTR_ERR(mcu_spi->mcu_class));
		return PTR_ERR(mcu_spi->mcu_class);
	}

	/* Create the base mcu char device */
	mcu_spi->mcu_major = register_chrdev(0, "mcu", &mcu_fops);
	if (mcu_spi->mcu_major < 0) {
		dev_err(&mcu_spi->spi->dev, "failed to mcu chardev: %d\n", mcu_spi->mcu_major);
		class_destroy(mcu_spi->mcu_class);
		return mcu_spi->mcu_major;
	}

	/* Bind mcu-ready interrupt handler */
	status = devm_request_irq(&mcu_spi->spi->dev, mcu_spi->ready_irq_num, mcu_ready_irq_handler, IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING, "MCU-READY", mcu_spi);
	if (status < 0) {
		dev_err(&mcu_spi->spi->dev, "failed to request mcu-ready threaded interrupt: %d\n", status);
		return status;
	}

	/* Bind the device node handler */
	mcu_spi->mcu_class->devnode = mcu_devnode;

	/* Create the file */
	status = device_create_file(&spi->dev, &dev_attr_board_id);
	if (status < 0)
		dev_warn(&spi->dev, "problem creating board id attr: %d\n", status);

	/* Log binding */
	dev_info(&spi->dev, "bound with chip state: %d\n", mcu_spi->chip_state);

	/* Should be all good */
	return 0;
}

static void mcu_spi_remove(struct spi_device *spi)
{
	struct mcu_spi *mcu_spi = spi_get_drvdata(spi);

	dev_dbg(&spi->dev, "%s invoked\n", __func__);

	device_remove_file(&spi->dev, &dev_attr_board_id);

	spi_imx_hook_device_wait_for_state(spi->controller, 0);

	/* Unregister the base chardev */
	unregister_chrdev(mcu_spi->mcu_major, "mcu");

	/* Destroy the class */
	class_destroy(mcu_spi->mcu_class);

	/* All good */
}

static const struct of_device_id mcu_spi_of_match[] =
{
	{
		.compatible = "shapertools,mcu-spi",
	},
	{
	}
};
MODULE_DEVICE_TABLE(of, mcu_spi_of_match);

static struct spi_driver mcu_spi_driver =
{
	.driver =
	{
		.name = "mcu-spi",
		.of_match_table = mcu_spi_of_match,
	},
	.probe = mcu_spi_probe,
	.remove = mcu_spi_remove,
};

module_spi_driver(mcu_spi_driver);

MODULE_DESCRIPTION("SPI Procotol Driver for ShaperTools MCU");
MODULE_AUTHOR("Stephen Street");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:mcu-spi");
