/*#define DEBUG*/

#include <linux/device.h>
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/spi/spi.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/binfmts.h>

#include "mcu-app.h"
#include "mcu-descriptor.h"
#include "mcu-spi.h"

static void space_buffer_release(struct kref *kref)
{
	struct space_buffer *buffer = container_of(kref, struct space_buffer, ref);
	struct mcu_app_device *dev = buffer->dev;
	kmem_cache_free(dev->space_cache, buffer);
}

static void space_buffer_ctor(void *obj)
{
	struct space_buffer *buffer = obj;
	kref_init(&buffer->ref);
}

static struct space_buffer *allocate_space_buffer(struct mcu_app_device *mcu_app_dev, gfp_t flags, unsigned int space_id)
{
	struct space_buffer *buffer = 0;
	struct space_descriptor *space_desc;

	space_desc = mcu_descriptor_find_space_by_id(mcu_app_dev->descriptor, space_id);
	if (!space_desc)
		return 0;

	buffer = kmem_cache_alloc(mcu_app_dev->space_cache, flags);
	if (!buffer)
		return 0;

	kref_init(&buffer->ref);
	buffer->dev = mcu_app_dev;
	buffer->space_id = space_id;
	buffer->space_size = space_desc->size;
	buffer->addr = 0;
	buffer->retries = 5;

	return buffer;
}

static inline struct space_buffer *space_buffer_get(struct space_buffer *buffer)
{
	kref_get(&buffer->ref);
	return buffer;
}

static inline void space_buffer_put(struct space_buffer *buffer)
{
	kref_put(&buffer->ref, space_buffer_release);
}

static void space_queue_init(struct space_buffer_queue *queue)
{
	spin_lock_init(&queue->fifo_lock);
	INIT_KFIFO(queue->fifo);
}

static void space_queue_destroy(struct space_buffer_queue *queue)
{
}

static struct space_buffer *space_queue_pop(struct space_buffer_queue *queue)
{
	struct space_buffer *buffer = 0;

	if (kfifo_out_spinlocked(&queue->fifo, &buffer, 1, &queue->fifo_lock) != 0)
		space_buffer_put(buffer);

	return buffer;
}

static void space_queue_push(struct space_buffer_queue *queue, struct space_buffer *buffer)
{
	unsigned long flags;
	struct space_buffer *discard;

	space_buffer_get(buffer);

	/* Add to queue making room if required */
	spin_lock_irqsave(&queue->fifo_lock, flags);
	while (kfifo_put(&queue->fifo, buffer) == 0) {
		if (kfifo_get(&queue->fifo, &discard))
			space_buffer_put(discard);
	}
	spin_unlock_irqrestore(&queue->fifo_lock, flags);
}

static inline bool space_queue_is_empty(const struct space_buffer_queue *queue)
{
	return kfifo_is_empty(&queue->fifo);
}

static void space_queue_flush_not_matching(struct space_buffer_queue *queue, unsigned int space_id)
{
	unsigned long flags;
	struct space_buffer *discard;

	spin_lock_irqsave(&queue->fifo_lock, flags);
	while (!space_queue_is_empty(queue)) {
		if (kfifo_peek(&queue->fifo, &discard) && discard->space_id != space_id) {
			if (kfifo_get(&queue->fifo, &discard))
				space_buffer_put(discard);
		} else
			break;

	}
	spin_unlock_irqrestore(&queue->fifo_lock, flags);
}

__attribute__((unused)) static int read_uint64(struct mcu_app_device *mcu_app_dev, unsigned int id, uint64_t *value)
{
	int status;
	status = mcu_app_read(mcu_app_dev, (id >> 24), (id & 0xffff), value, sizeof(uint64_t));
	if (status != sizeof(uint64_t))
		return status < 0 ? status : -EIO;
	return 0;
}

static int write_uint64(struct mcu_app_device *mcu_app_dev, unsigned int id, uint64_t value)
{
	int status;
	status = mcu_app_write(mcu_app_dev, (id >> 24), (id & 0xffff), &value, sizeof(uint64_t));
	if (status != sizeof(uint64_t))
		return status < 0 ? status : -EIO;
	return 0;
}

static void read_complete(struct mcu_spi_transaction *trans, size_t count, int status)
{
	unsigned long flags;
	struct mcu_app_file *file;
	struct space_buffer *buffer = container_of(trans, struct space_buffer, trans);

	/* Did we ge a good result? */
	if (status < 0) {
		dev_warn(buffer->dev->mcu_spi_dev.this, "problem async reading of space %u status: %d count: %u\n", buffer->space_id, status, count);

		/* Requeue if we have not exceeded our retries */
		if (status == -ETIMEDOUT && buffer->retries-- > 0) {
			mcu_spi_send_reset(&buffer->dev->mcu_spi_dev);
			status = mcu_spi_async(&buffer->dev->mcu_spi_dev, &buffer->trans);
			if (status == 0)
				return;
		}

		/* Abandon */
		space_buffer_put(buffer);
		return;
	}

	/* Post buffer to any waiters */
	spin_lock_irqsave(&buffer->dev->stream_waiters[buffer->space_id].waiters_lock, flags);
	list_for_each_entry(file, &buffer->dev->stream_waiters[buffer->space_id].waiters, queue_node) {
		space_queue_push(&file->input_queue, buffer);
		wake_up(&file->ready_wq);
	}
	spin_unlock_irqrestore(&buffer->dev->stream_waiters[buffer->space_id].waiters_lock, flags);
}

static int read_space_async(struct mcu_app_device *mcu_app_dev, unsigned int space_id, unsigned int addr, size_t count)
{
	int status;
	struct space_buffer *buffer;

	/* Allocate a space buffer */
	buffer = allocate_space_buffer(mcu_app_dev, GFP_ATOMIC, space_id);

	if (buffer == 0) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem allocate space buffer for %u\n", space_id);
		return -ENOMEM;
	}

	/* Initialize the command block */
	buffer->cmd.flags = CMD_READ;
	buffer->cmd.space = space_id;
	buffer->cmd.size = count;
	buffer->cmd.addr = addr;

	/* Initialize the transaction */
	mcu_spi_trans_init(&buffer->trans, &buffer->cmd, sizeof(buffer->cmd), buffer->data, buffer->space_size, read_complete);

	/* Launch the async transaction */
	status = mcu_spi_async(&mcu_app_dev->mcu_spi_dev, &buffer->trans);
	if (status < 0) {
		space_buffer_put(buffer);
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem launching read of space %u: %d\n", space_id, status);
		return status;
	}

	/* On its way */
	return 0;
}

static int mcu_int_event_handler(unsigned int event, void *context)
{
	int status;
	struct space_descriptor *space_desc;
	unsigned long long interrupts_active;
	unsigned int space_id;
	struct mcu_app_device *mcu_app_dev = context;

	/* Read the mcu status */
	dev_dbg(mcu_app_dev->mcu_spi_dev.this, "reading mcu status\n");
	status = mcu_app_read(mcu_app_dev, MCU_APP_SPACE_ID, 0, &mcu_app_dev->mcu_status, sizeof(uint64_t) * 3);
	if (status < 0) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed to read pending interrupts, resetting mcu state:%d\n", status);
		if (status != -ETIMEDOUT)
			return status;

		/* Send a reset and retry */
		mcu_spi_send_reset(&mcu_app_dev->mcu_spi_dev);
		return 0;
	}

	dev_dbg(mcu_app_dev->mcu_spi_dev.this, "handling pending: 0x%016llx\n", mcu_app_dev->mcu_status.interrupt_pending);

	/* Only handled enabled interrupts */
	if (mcu_app_dev->mcu_status.interrupt_pending != 0) {

		/* Launch and asycn reads */
		interrupts_active = mcu_app_dev->mcu_status.interrupt_pending & mcu_app_dev->mcu_status.interrupt_enabled;
		while (interrupts_active != 0) {

			/* Extract the current space id */
			space_id = fls64(interrupts_active) - 1;

			/* Is the space status is clear or the read list is empty, skip */
			dev_dbg(mcu_app_dev->mcu_spi_dev.this, "read space %d", space_id);
			if ((mcu_app_dev->mcu_status.space_status & (1ULL << space_id)) != 0 && !list_empty(&mcu_app_dev->stream_waiters[space_id].waiters)) {
				space_desc = mcu_descriptor_find_space_by_id(mcu_app_dev->descriptor, space_id);
				if (space_desc) {
					read_space_async(mcu_app_dev, space_id, 0, space_desc->size);
				} else
					dev_warn(mcu_app_dev->mcu_spi_dev.this, "could not find space descriptor: %u", space_id);
			} else if (list_empty(&mcu_app_dev->stream_waiters[space_id].waiters)) {
				/* No handler, disable the interrupt */
				dev_warn(mcu_app_dev->mcu_spi_dev.this, "no handler, disabling space %u interrupt", space_id);
				status = write_uint64(mcu_app_dev, MCU_APP_INTERRUPT_DISABLE, 1ULL << space_id);
				if (status < 0)
					dev_err(mcu_app_dev->mcu_spi_dev.this, "could not set 'space.interrupts_disable': %d\n", status);
			}

			/* Move to the next set bit */
			interrupts_active &= ~(1ULL << space_id);
		}

		/* Clear the pending interrupts */
		dev_dbg(mcu_app_dev->mcu_spi_dev.this, "writing clear active: 0x%016llx\n", mcu_app_dev->mcu_status.interrupt_pending & mcu_app_dev->mcu_status.interrupt_enabled);
		status = write_uint64(mcu_app_dev, MCU_APP_INTERRUPT_CLEAR, mcu_app_dev->mcu_status.interrupt_pending);
		if (status < 0) {
			dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed to clear pending interrupts, resetting mcu state:%d\n", status);
			if (status != -ETIMEDOUT)
				return status;

			/* Send reset and try again */
			mcu_spi_send_reset(&mcu_app_dev->mcu_spi_dev);
		}
	}

	dev_dbg(mcu_app_dev->mcu_spi_dev.this, "reading handling interrupts done\n");

	/* All done here */
	return 0;
}

static ssize_t mcu_reset_state_machine(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct mcu_app_device *mcu_app_dev = to_mcu_app_device(dev_get_drvdata(dev));

	mcu_spi_send_reset(&mcu_app_dev->mcu_spi_dev);

	return count;
}

static void bind_stream_reader(struct mcu_app_file *mcu_file, unsigned int space_id)
{
	int status;
	unsigned long flags;

	/* Protect the readers list */
	spin_lock_irqsave(&mcu_file->mcu_app_dev->stream_waiters[space_id].waiters_lock, flags);

	/* If we are already bound to the read list, remove */
	if (!list_empty(&mcu_file->queue_node))
		list_del(&mcu_file->queue_node);

	/* Add it to the new read read list */
	list_add(&mcu_file->queue_node, &mcu_file->mcu_app_dev->stream_waiters[space_id].waiters);

	/* Done */
	spin_unlock_irqrestore(&mcu_file->mcu_app_dev->stream_waiters[space_id].waiters_lock, flags);

	/* Enable the interrupt */
	dev_dbg(mcu_file->mcu_app_dev->mcu_spi_dev.this, "enabling interrupt: 0x%016llx\n", 1ULL << space_id);
	status = write_uint64(mcu_file->mcu_app_dev, MCU_APP_INTERRUPT_ENABLE, 1ULL << space_id);
	if (status < 0)
		dev_err(mcu_file->mcu_app_dev->mcu_spi_dev.this, "could not set 'space.interrupts_enable': %d\n", status);

}

static void unbind_stream_reader(struct mcu_app_file *mcu_file, unsigned int space_id)
{
	int status;
	unsigned long flags;

	/* Protect the readers list */
	spin_lock_irqsave(&mcu_file->mcu_app_dev->stream_waiters[space_id].waiters_lock, flags);

	/* If we are already bound to the read list, otherwise ignore */
	if (!list_empty(&mcu_file->queue_node)) {
		list_del(&mcu_file->queue_node);
		INIT_LIST_HEAD(&mcu_file->queue_node);
	}

	/* Done */
	spin_unlock_irqrestore(&mcu_file->mcu_app_dev->stream_waiters[space_id].waiters_lock, flags);

	/* Disable the interrupt */
	dev_dbg(mcu_file->mcu_app_dev->mcu_spi_dev.this, "disabling interrupt: 0x%016llx\n", 1ULL << space_id);
	status = write_uint64(mcu_file->mcu_app_dev, MCU_APP_INTERRUPT_DISABLE, 1ULL << space_id);
	if (status < 0)
		dev_err(mcu_file->mcu_app_dev->mcu_spi_dev.this, "could not set 'space.interrupts_disable': %d\n", status);
}

static ssize_t mcu_app_read_direct(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	ssize_t read_size;
	ssize_t amount = 0;
	struct mcu_app_file *mcu_file = file->private_data;
	unsigned int space = MCU_APP_TO_SPACE_NUM(*offset);
	unsigned int addr = MCU_APP_TO_SPACE_ADDR(*offset);

	/* TODO Need to check that the space is active */

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

	/* Make sure at least one seek was done */
	if (!mcu_file->space_desc || MCU_APP_TO_SPACE_NUM(*offset) != mcu_file->space_desc->id) {
		mcu_file->space_desc = mcu_descriptor_find_space_by_id(mcu_file->mcu_app_dev->descriptor, MCU_APP_TO_SPACE_NUM(*offset));
		if (!mcu_file->space_desc) {
			amount = -EBADF;
			goto error_unlock;
		}
	}

	/* Is addr outside of the space return zero read, return EOF */
	if (addr > mcu_file->space_desc->size)
		goto error_unlock;

	/* Trim the count to work with the room after the address */
	count = addr + count > mcu_file->space_desc->size ? mcu_file->space_desc->size - addr : count;

	/* Try to read everything requested in multiples of the block size */
	while (amount < count) {

		/* How should we read?*/
		read_size = count - amount < PAGE_SIZE ? count - amount : PAGE_SIZE;

		/* Launch the read */
		read_size =  mcu_app_read(mcu_file->mcu_app_dev, space, *offset & 0x00ffffff, mcu_file->bounce, read_size);
		if (read_size < 0) {
			amount = read_size;
			break;
		}

		/* Copy to the user buffer */
		if (copy_to_user(buf + amount, mcu_file->bounce, read_size)) {
			amount = -EFAULT;
			break;
		}

		/* Update the amount and position */
		amount += read_size;
		*offset += read_size;
	}

error_unlock:
	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	/* Return the amount read or error */
	return amount;
}

static ssize_t mcu_app_read_stream(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	ssize_t amount = 0;
	struct space_buffer *buffer = 0;
	struct mcu_app_file *mcu_file = file->private_data;

	/* We should already have a space descriptor */
	if (!mcu_file->space_desc)
		return -EINVAL;

	/* TODO Need to check that the space is active */

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

	/* Trim the count s */
	count = count > mcu_file->space_desc->size ? mcu_file->space_desc->size : count;

	if ((file->f_flags & O_NONBLOCK) && space_queue_is_empty(&mcu_file->input_queue)) {
		amount = -EAGAIN;
		goto error_unlock;
	}

	/* Wait for a buffer */
	while (buffer == 0) {

		if (wait_event_interruptible(mcu_file->ready_wq, !space_queue_is_empty(&mcu_file->input_queue))) {
			amount = -ERESTARTSYS;
			goto error_unlock;
		}

		/* Get the buffer */
		buffer = space_queue_pop(&mcu_file->input_queue);
	}

	if (buffer == 0) {
		printk(KERN_ERR "space queue buffer is null\n");
		amount = -EFAULT;
		goto error_unlock;
	}

	/* Copy to the user buffer using and offset */
	if (copy_to_user(buf, buffer->data, count)) {
		amount = -EFAULT;
		goto error_unlock;
	}

	/* Update the amount and force the offset to zero */
	amount = count;
	*offset += count;

error_unlock:

	/* Release the buffer is we got one from the queue */
	if (buffer)
		space_buffer_put(buffer);

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	/* Return the amount read or error */
	return amount;
}

static ssize_t mcu_app_file_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	struct mcu_app_file *mcu_file = file->private_data;
	return mcu_file->read(file, buf, count, offset);
}

static ssize_t mcu_app_file_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	ssize_t write_size;
	ssize_t amount = 0;
	struct mcu_app_file *mcu_file = file->private_data;
	unsigned int space = MCU_APP_TO_SPACE_NUM(*offset);
	unsigned int addr = MCU_APP_TO_SPACE_ADDR(*offset);

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

	/* Make sure at least one seek was done */
	if (!mcu_file->space_desc || MCU_APP_TO_SPACE_NUM(*offset) != mcu_file->space_desc->id) {
		mcu_file->space_desc = mcu_descriptor_find_space_by_id(mcu_file->mcu_app_dev->descriptor, MCU_APP_TO_SPACE_NUM(*offset));
		if (!mcu_file->space_desc) {
			amount = -EBADF;
			goto error_unlock;
		}
	}

	/* Is addr outside of the space? */
	if (addr > mcu_file->space_desc->size) {
		amount = -ENOSPC;
		goto error_unlock;
	}

	/* Trim the count to work with the room after the address */
	count = addr + count > mcu_file->space_desc->size ? mcu_file->space_desc->size - addr : count;

	/* Try to read everything requested in multiples of the block size */
	while (amount < count) {

		/* How should we write?*/
		write_size = count - amount < PAGE_SIZE ? count - amount : PAGE_SIZE;

		/* Copy data into buffer */
		if (copy_from_user(mcu_file->bounce, buf + amount, write_size)) {
			amount = -EFAULT;
			break;
		}

		/* Launch the write */
		write_size =  mcu_app_write(mcu_file->mcu_app_dev, space, *offset & 0x00ffffff, mcu_file->bounce, write_size);
		if (write_size < 0) {
			amount = write_size;
			break;
		}

		/* Update the amount and position */
		amount += write_size;
		*offset += write_size;
	}

error_unlock:

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	/* Return the amount written or error */
	return amount;
}

static unsigned int mcu_app_file_poll(struct file *file, poll_table *wait)
{
	struct mcu_app_file *mcu_file = file->private_data;
	unsigned int mask = POLLOUT | POLLIN;

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

	/* Initialize the wait */
	poll_wait(file, &mcu_file->ready_wq, wait);

	/* Are we in stream mode, if so is the queue empty, strip off POLLIN */
	if (mcu_file->space_desc && space_queue_is_empty(&mcu_file->input_queue))
		mask &= ~POLLIN;

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	/* All done */
	return mask;
}

static loff_t mcu_app_llseek_direct(struct file *file, loff_t offset, int whence)
{
	loff_t newpos = 0;
	int status = 0;
	struct mcu_app_file *mcu_file = file->private_data;

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

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
		status = -EINVAL;
	}

	/* Check the new position */
	if (newpos < 0 || MCU_APP_TO_SPACE_NUM(newpos) > MCU_APP_MAX_SPACES) {
		status = -EINVAL;
		goto error_unlock_file;
	}

	/* Do we need to rebind reader list? */
	if (MCU_APP_TO_SPACE_NUM(newpos) != MCU_APP_TO_SPACE_NUM(file->f_pos)) {

		/* Look the match descriptor or fail */
		mcu_file->space_desc = mcu_descriptor_find_space_by_id(mcu_file->mcu_app_dev->descriptor, MCU_APP_TO_SPACE_NUM(newpos));
		if (!mcu_file->space_desc) {
			dev_warn(mcu_file->mcu_app_dev->mcu_spi_dev.this, "llseek to bad space: %lld\n", MCU_APP_TO_SPACE_NUM(newpos));
			status = -ENOENT;
			goto error_unlock_file;
		}
	}

	/* Update the file position */
	file->f_pos = newpos;

error_unlock_file:

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	return status < 0 ? status : newpos;
}

static loff_t mcu_app_llseek_stream(struct file *file, loff_t offset, int whence)
{
	loff_t newpos = 0;
	int status = 0;
	struct mcu_app_file *mcu_file = file->private_data;

	/* Lock the file */
	if (mutex_lock_interruptible(&mcu_file->lock) < 0)
		return -ERESTARTSYS;

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
		status = -EINVAL;
	}

	/* Check the new position */
	if (newpos > 0 && MCU_APP_TO_SPACE_NUM(newpos) == mcu_file->bound_space) {
		file->f_pos = newpos;
	} else
		status = -EINVAL;

	/* Release the file lock */
	mutex_unlock(&mcu_file->lock);

	return status < 0 ? status : newpos;
}

static loff_t mcu_app_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct mcu_app_file *mcu_file = file->private_data;
	return mcu_file->llseek(file, offset, whence);
}

static long mcu_app_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int status = 0;
/*	unsigned long value = arg;*/
	unsigned long *value_ptr = (unsigned long *)arg;
	int i;
	struct mcu_ioctl_setup_spi ioctl_setup;
	struct mcu_app_file *mcu_file = file->private_data;
	struct mcu_descriptor *mcu_desc;
	struct mcu_ioctl_desc_info *desc_info = (struct mcu_ioctl_desc_info *)arg;
	struct mcu_ioctl_space_desc *desc = (struct mcu_ioctl_space_desc *)arg;
	struct mcu_ioctl_buffer mcu_buffer;
	char desc_name[MCU_DESC_MAX_NAME_SIZE];
	unsigned int id;
	const struct space_descriptor *space_desc;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != _MCU_IOCTL_TYPE)
		return -ENOTTY;

	/* Check direction */
	if ((_IOC_DIR(cmd) & _IOC_READ) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	/* Lock the file */
	mutex_lock(&mcu_file->lock);

	/* Dispatch */
	switch (_IOC_NR(cmd)) {

	case _MCU_IOCTL_CMD_RESET:

		/* Range check and forward*/
		if (arg <= MCU_MODE_SRAM)
			status = mcu_spi_reset(&mcu_file->mcu_app_dev->mcu_spi_dev, (int)arg);
		else
			status = -EINVAL;

		break;

	case _MCU_IOCTL_CMD_SETUP_SPI:

		/* Extract the ioctl */
		if (copy_from_user(&ioctl_setup, (struct mcu_ioctl_setup_spi __user *)arg, sizeof(struct mcu_ioctl_setup_spi)) == 0)
			status = mcu_spi_setup(&mcu_file->mcu_app_dev->mcu_spi_dev, ioctl_setup.speed_hz, ioctl_setup.bits_per_word, ioctl_setup.mode);
		else
			status = -EFAULT;

		break;

		dev_info(mcu_file->mcu_app_dev->mcu_spi_dev.this, "reseting mcu\n");

		/* Range check and forward*/
		status = mcu_spi_reset(&mcu_file->mcu_app_dev->mcu_spi_dev, MCU_MODE_FLASH);
		if (status != 0)
			break;

		/* TODO Verify the descriptor matches */

		break;

	case _MCU_IOCTL_CMD_GET_NUM_DESC:

		mcu_desc = mcu_file->mcu_app_dev->descriptor;
		if (!mcu_desc) {
			status = -ENOENT;
			break;
		}

		if (put_user(mcu_desc->num_spaces, value_ptr) != 0)
			status = -EFAULT;

		break;

	case _MCU_IOCTL_CMD_GET_DESC_LIST:

		mcu_desc = mcu_file->mcu_app_dev->descriptor;
		if (mcu_desc) {

			for (i = 0; i < mcu_desc->num_spaces; ++i) {

				if (copy_to_user(desc_info[i].desc_name, mcu_desc->spaces[i]->name, strlen(mcu_desc->spaces[i]->name) + 1) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(mcu_desc->spaces[i]->id, &desc_info[i].desc_id) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(sizeof(struct mcu_ioctl_space_desc) + (sizeof(struct mcu_ioctl_field_desc) * mcu_desc->spaces[i]->num_fields), &desc_info[i].desc_size) != 0) {
					status = -EFAULT;
					break;
				}
			}

		} else
			status = -ENOENT;

		break;

	case _MCU_IOCTL_CMD_GET_DESC:

		mcu_desc = mcu_file->mcu_app_dev->descriptor;
		if (mcu_desc) {

			/* First check if the caller provided a name */
			if (strnlen_user(desc->name, MAX_ARG_STRLEN) > 1) {

				if (strncpy_from_user(desc_name, desc->name, MCU_DESC_MAX_NAME_SIZE) != 0) {
					status = -EFAULT;
					break;
				}

				space_desc = mcu_descriptor_find_space_by_name(mcu_desc, desc_name);
				if (!space_desc) {
					status = -ENOENT;
					break;
				}

			/* Nope try the id */
			} else {

				if (get_user(id, &desc->id) != 0) {
					status = -EFAULT;
					break;
				}

				space_desc = mcu_descriptor_find_space_by_id(mcu_desc, id);
				if (!space_desc) {
					status = -ENOENT;
					break;
				}
			}

			if (copy_to_user(desc->name, space_desc->name, MCU_DESC_MAX_NAME_SIZE) != 0) {
				status = -EFAULT;
				break;
			}

			if (put_user(space_desc->id, &desc->id) != 0) {
				status = -EFAULT;
				break;
			}

			if (put_user(space_desc->size, &desc->size) != 0) {
				status = -EFAULT;
				break;
			}

			if (put_user(space_desc->num_fields, &desc->num_fields) != 0) {
				status = -EFAULT;
				break;
			}

			for (i = 0; i < space_desc->num_fields; ++i) {

				if (copy_to_user(desc->fields[i].name, space_desc->fields[i].name, MCU_DESC_MAX_NAME_SIZE) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(space_desc->fields[i].type, &desc->fields[i].type) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(space_desc->fields[i].size, &desc->fields[i].size) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(space_desc->fields[i].offset, &desc->fields[i].offset) != 0) {
					status = -EFAULT;
					break;
				}
			}

		} else
			status = -ENOENT;

		break;

	case _MCU_IOCTL_CMD_SPI_RESET:

		status = mcu_spi_send_reset(&mcu_file->mcu_app_dev->mcu_spi_dev);

		break;

	case _MCU_IOCTL_CMD_MODE:

		if (arg != 0) {

			/* Mask sure the space id if valid */
			mcu_file->space_desc = mcu_descriptor_find_space_by_id(mcu_file->mcu_app_dev->descriptor, arg);
			if (mcu_file->space_desc) {

				/* Mark as bound, adjust the file pointer to the new space and attach interrupt read */
				mcu_file->bound_space = arg;
				file->f_pos = 0;
				mcu_file->read = mcu_app_read_stream;
				mcu_file->llseek = mcu_app_llseek_stream;
				bind_stream_reader(mcu_file, mcu_file->bound_space);

				/* Flush the queue of any old buffers */
				space_queue_flush_not_matching(&mcu_file->input_queue, mcu_file->bound_space);

				/* Kick the mcu to prime the input queue */
				status = write_uint64(mcu_file->mcu_app_dev, MCU_APP_INTERRUPT_SET, (1ULL << arg));
				if (status < 0) {

					/* Unbind */
					unbind_stream_reader(mcu_file, arg);
					mcu_file->bound_space = 0;
					mcu_file->read = mcu_app_read_direct;
					mcu_file->llseek = mcu_app_llseek_direct;

					/* Flush the queue of any old buffers */
					space_queue_flush_not_matching(&mcu_file->input_queue, 0);
				}

			} else
				status = -ENOENT;

		/* Unbind leaving the file position at the current position */
		} else if (mcu_file->bound_space != 0) {

			/* Unbind */
			unbind_stream_reader(mcu_file, arg);
			mcu_file->bound_space = 0;
			mcu_file->read = mcu_app_read_direct;
			mcu_file->llseek = mcu_app_llseek_direct;

			/* Flush the queue of any old buffers */
			space_queue_flush_not_matching(&mcu_file->input_queue, 0);
		}

		break;

	case _MCU_IOCTL_CMD_CAPTURE:

		/* Extract the capture data */
		if (copy_from_user(&mcu_buffer, (struct mcu_ioctl_buffer __user *)arg, sizeof(struct mcu_ioctl_buffer)) == 0) {

			if (mcu_buffer.flags & MCU_BUFFER_MMAP) {

				/* Memory mapped io */
				status = mcu_app_mmap_capture(file, mcu_buffer.flags, MCU_APP_TO_SPACE_NUM(mcu_buffer.offset), MCU_APP_TO_SPACE_ADDR(mcu_buffer.offset), mcu_buffer.size, &mcu_buffer.timestamp);
				if (status < 0)
					break;

			} else {

				/* User io */
				status = mcu_app_capture(mcu_file->mcu_app_dev, MCU_APP_TO_SPACE_NUM(mcu_buffer.offset), MCU_APP_TO_SPACE_ADDR(mcu_buffer.offset), mcu_file->bounce, mcu_buffer.size, &mcu_buffer.timestamp);
				if (status < 0)
					break;

				/* Copy to the user buffer */
				if (copy_to_user(mcu_buffer.buffer, mcu_file->bounce, status)) {
					status = -EFAULT;
					break;
				}

			}

			/* Copy the timestamp */
			if (copy_to_user(&((struct mcu_ioctl_buffer __user *)arg)->timestamp, &mcu_buffer.timestamp, sizeof(mcu_buffer.timestamp)))
				status = -EFAULT;

		} else
			status = -EFAULT;

		break;

	case _MCU_IOCTL_CMD_FLUSH:

		/* Extract the flush data */
		if (copy_from_user(&mcu_buffer, (struct mcu_ioctl_buffer __user *)arg, sizeof(struct mcu_ioctl_buffer)) == 0) {

			/* Memory mapped io */
			if (mcu_buffer.flags & MCU_BUFFER_MMAP) {

				/* Memory mapped io */
				status = mcu_app_mmap_flush(file, mcu_buffer.flags, MCU_APP_TO_SPACE_NUM(mcu_buffer.offset), MCU_APP_TO_SPACE_ADDR(mcu_buffer.offset), mcu_buffer.size, &mcu_buffer.timestamp);
				if (status < 0)
					break;

			} else {

				/* Copy the user buffer */
				if (copy_from_user(mcu_file->bounce, mcu_buffer.buffer, mcu_buffer.size)) {
					status = -EFAULT;
					break;
				}

				/* User io */
				status = mcu_app_flush(mcu_file->mcu_app_dev, MCU_APP_TO_SPACE_NUM(mcu_buffer.offset), MCU_APP_TO_SPACE_ADDR(mcu_buffer.offset), mcu_file->bounce, mcu_buffer.size, &mcu_buffer.timestamp);
				if (status < 0)
					break;
			}

			/* Copy the timestamp */
			if (copy_to_user(&((struct mcu_ioctl_buffer __user *)arg)->timestamp, &mcu_buffer.timestamp, sizeof(mcu_buffer.timestamp)))
				status = -EFAULT;

		} else
			status = -EFAULT;

		break;

	default:

		/* Opp unknown ioctl */
		dev_warn(mcu_file->mcu_app_dev->mcu_spi_dev.this, "invalid cmd: %u\n", _IOC_NR(cmd));
		status = -ENOTTY;
		break;
	}

	/* Unlock the file */
	mutex_unlock(&mcu_file->lock);

	/* May all good, who knows, not me */
	return status;
}

int initialize_space_maps(struct space_descriptor *space_desc, void *context)
{
	struct space_map *mappings = context;

	mappings[space_desc->id].buffer = 0;
	mappings[space_desc->id].desc = space_desc;
	mappings[space_desc->id].num_mappings = 0;

	return 0;
}

static int mcu_app_file_open(struct inode *inode, struct file *file)
{
	int status;
	struct mcu_spi_device *mcu_spi_dev = file->private_data;
	struct mcu_app_device *mcu_app_dev = container_of(mcu_spi_dev, struct mcu_app_device, mcu_spi_dev);
	struct mcu_app_file *mcu_file;

	/* Allocate the the new file structure */
	mcu_file = kzalloc(sizeof(struct mcu_app_file), GFP_KERNEL);
	if (!mcu_file)
		return -ENOMEM;

	/* Initialize the local space mappings */
	mcu_descriptor_foreach_space(mcu_app_dev->descriptor, initialize_space_maps, mcu_file->space_maps);

	/* Allocate the bounce buffers */
	mcu_file->bounce = (void *)get_zeroed_page(GFP_KERNEL);
	if (!mcu_file->bounce) {
		status = -ENOMEM;
		goto error_release_file;
	}

	/* Initialize the lock queues and lists */
	mutex_init(&mcu_file->lock);
	init_waitqueue_head(&mcu_file->ready_wq);
	space_queue_init(&mcu_file->input_queue);
	INIT_LIST_HEAD(&mcu_file->queue_node);

	/* Initialize as direct reader */
	mcu_file->read = mcu_app_read_direct;
	mcu_file->llseek = mcu_app_llseek_direct;

	/* Save the device */
	mcu_file->mcu_app_dev = mcu_app_dev;

	/* Update the file private data */
	file->private_data = mcu_file;

	/* All good */
	return 0;

error_release_file:
	kfree(mcu_file);

	return status;
}

static int mcu_app_file_release(struct inode *inode, struct file *file)
{
	struct mcu_app_file *mcu_file = file->private_data;

	/* Unbind reader if bound */
	if (mcu_file->bound_space != 0)
		unbind_stream_reader(mcu_file, mcu_file->bound_space);

	/* Release the bounce buffers */
	free_page((unsigned long)mcu_file->bounce);

	/* Destroy the queues */
	space_queue_destroy(&mcu_file->input_queue);

	/* Release the file */
	kfree(mcu_file);

	/* Always good */
	return 0;
}


int find_max_space_size(struct space_descriptor *space_desc, void *context)
{
	size_t *max_size = context;

	/* TODO User non-cachable flags from descriptor */
	if (strcmp(space_desc->name, "flash") != 0 && strcmp(space_desc->name, "loopback"))
		*max_size = max(space_desc->size, *max_size);
	return 0;
}

static const struct file_operations mcu_app_fops =
{
	.owner = THIS_MODULE,
	.open = mcu_app_file_open,
	.release = mcu_app_file_release,
	.read = mcu_app_file_read,
	.write = mcu_app_file_write,
	.poll = mcu_app_file_poll,
	.llseek = mcu_app_file_llseek,
	.unlocked_ioctl = mcu_app_file_ioctl,
	.mmap = mcu_app_mmap,
};

static int load_descriptor(struct mcu_app_device *mcu_app_dev)
{
	ssize_t status;
	void *desc_buf = 0;
	uint16_t desc_len[2];

	dev_info(mcu_app_dev->mcu_spi_dev.this, "updating descriptor\n");

	/* Now read the descriptor len */
	status = mcu_app_read(mcu_app_dev, 0, 0, desc_len, sizeof(desc_len));
	if (status != sizeof(desc_len)) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem reading descriptor length: %d\n", status);
		return -EIO;
	}

	/* Did we get a valid descriptor? */
	if ((desc_len[0] ^ desc_len[1]) != MCU_DESCRIPTOR_LEN_MAGIC) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "bad descriptor length, maybe the MCU need flashing: 0x%04hx, 0x%04hx\n", desc_len[0], desc_len[1]);
		return -EIO;
	}

	dev_dbg(mcu_app_dev->mcu_spi_dev.this, "descriptor len: %u\n", desc_len[0]);

	/* Allocate a buffer for the descriptor */
	desc_buf = kzalloc(desc_len[0], GFP_KERNEL);
	if (!desc_buf) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem allocating descriptor buffer of length: %d\n", desc_len[0]);
		return -ENOMEM;
	}

	/* Read the descriptor */
	status = mcu_app_read(mcu_app_dev, 0, sizeof(uint32_t), desc_buf, desc_len[0]);
	if (status != desc_len[0]) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem reading descriptor of length %d: %d \n", desc_len[0], status);
		status = -EIO;
		goto error_free_desc_buf;
	}

	dev_dbg(mcu_app_dev->mcu_spi_dev.this, "read descriptor of len: %u\n", desc_len[0]);

	/* Try to decode the descriptor */
	mcu_app_dev->descriptor = mcu_descriptor_new(mcu_app_dev, desc_buf, desc_len[0]);
	if (!mcu_app_dev->descriptor) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem decoding descriptor of length %d\n", desc_len[0]);
		goto error_free_desc_buf;
	}

	dev_dbg(mcu_app_dev->mcu_spi_dev.this, "loaded %u space descriptors\n", mcu_app_dev->descriptor->num_spaces);

	/* All good */
	status = 0;

error_free_desc_buf:
	if (desc_buf)
		kfree(desc_buf);

	dev_info(mcu_app_dev->mcu_spi_dev.this, "done updating descriptor: %d\n", status);

	return status;
}

static void mcu_app_exit(void)
{
	int status;

	/* Retrieve the device from the shim */
	struct mcu_spi_device *mcu_spi_dev = mcu_spi_find("app");
	struct mcu_app_device *mcu_app_dev = container_of(mcu_spi_dev, struct mcu_app_device, mcu_spi_dev);

	/* Release the mcu descriptor */
	mcu_descriptor_put(mcu_app_dev->descriptor);

	/* Disable all the interrupts */
	status = write_uint64(mcu_app_dev, MCU_APP_INTERRUPT_DISABLE, 0xffffffffffffffff);
	if (status < 0)
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "could not set 'space.interrupts_disable': %d\n", status);

	/* Unbind event notification */
	mcu_spi_unregister_event(mcu_spi_dev, &mcu_app_dev->mcu_event);

	/* Remove the reset state attribute */
	status = mcu_spi_remove_attr(mcu_spi_dev, "mcu-reset-state");
	if (status != 0)
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem removing mcu-reset-state sysfs attribute: %d\n", status);

	/* Destroy the cache */
	kmem_cache_destroy(mcu_app_dev->space_cache);

	/* Unregister */
	mcu_spi_unregister(mcu_spi_dev);

	/* Free it */
	kfree(mcu_app_dev);
}

static __init int mcu_app_init(void)
{
	int i;
	struct mcu_app_device *mcu_app_dev;
	size_t max_space_size = 0;
	int status = 0;

	/* Allocate the device */
	mcu_app_dev = kzalloc(sizeof(struct mcu_app_device), GFP_KERNEL);
	if (!mcu_app_dev)
		return -ENOMEM;

	/* Initialize the device */
	mcu_app_dev->mcu_spi_dev.name = "app";
	mcu_app_dev->mcu_spi_dev.ops = &mcu_app_fops;

	/* Register the new device */
	status = mcu_spi_register(&mcu_app_dev->mcu_spi_dev);
	if (status < 0) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "problem registering new mcu spi device: %d\n", status);
		goto error_free_mcu_app_dev;
	}

	/*  Add reset spi mcu state machine attr */
	status = mcu_spi_add_attr(&mcu_app_dev->mcu_spi_dev, "mcu-reset-state", 0, mcu_reset_state_machine, 0);
	if (status != 0) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "problem adding mcu-reset-state sysfs attribute: %d\n", status);
		goto error_unregister;
	}

	/* Initialize locks and lists */
	spin_lock_init(&mcu_app_dev->descriptor_lock);
	for (i = 0; i < MCU_APP_MAX_SPACES; ++i) {
		spin_lock_init(&mcu_app_dev->stream_waiters[i].waiters_lock);
		INIT_LIST_HEAD(&mcu_app_dev->stream_waiters[i].waiters);
	}
	mutex_init(&mcu_app_dev->space_maps_lock);

	/* Register for mcu events */
	mcu_app_dev->mcu_event.notify = mcu_int_event_handler;
	mcu_app_dev->mcu_event.context = mcu_app_dev;
	mcu_app_dev->mcu_event.event_mask = MCU_SPI_INT_EVENT;
	INIT_LIST_HEAD(&mcu_app_dev->mcu_event.node);

	/* Ensure that the mcu is in flash mode */
	if (mcu_spi_chip_state(&mcu_app_dev->mcu_spi_dev) != MCU_SPI_MODE_FLASH) {
		dev_info(mcu_app_dev->mcu_spi_dev.this, "resetting MCU to flash mode\n");
		status = mcu_spi_reset(&mcu_app_dev->mcu_spi_dev, MCU_SPI_MODE_FLASH);
		if (status < 0) {
			dev_err(mcu_app_dev->mcu_spi_dev.this, "problem resetting mcu: %d\n", status);
			goto error_remove_reset_state_attr;
		}
	} else {
		/* Reset the mcu spi interface */
		status = mcu_spi_send_reset(&mcu_app_dev->mcu_spi_dev);
		if (status != 0) {
			dev_err(mcu_app_dev->mcu_spi_dev.this, "problem sending mcu spi reset: %d\n", status);
			goto error_remove_reset_state_attr;
		}
	}

	/* Try to load he mcu descriptor */
	status = load_descriptor(mcu_app_dev);
	if (status < 0) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "problem creating mcu descriptor, try resetting the mcu: %d\n", status);
		goto error_remove_reset_state_attr;
	}

	/* Read the complete mcu status */
	status = mcu_app_read(mcu_app_dev, MCU_APP_SPACE_ID, 0, &mcu_app_dev->mcu_status, sizeof(mcu_app_dev->mcu_status));
	if (status != sizeof(mcu_app_dev->mcu_status)) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "could not read 'space': %d\n", status);
		goto error_destroy_descriptor;
	}

	/* Initialize the shared space mappings */
	mcu_descriptor_foreach_space(mcu_app_dev->descriptor, initialize_space_maps, mcu_app_dev->space_maps);

	/* Get the max size of any space and round up to a multiple of uint64 */
	mcu_descriptor_foreach_space(mcu_app_dev->descriptor, find_max_space_size, &max_space_size);
	mcu_app_dev->space_buffer_size = roundup(max_space_size, sizeof(uint64_t));

	/* Initialize the cache */
	mcu_app_dev->space_cache = kmem_cache_create("MCU_APP_DEV", sizeof(struct space_buffer) + mcu_app_dev->space_buffer_size, 0, 0, space_buffer_ctor);
	if (!mcu_app_dev->space_cache) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "could not allocate space cache with size %u\n", mcu_app_dev->space_buffer_size);
		goto error_destroy_descriptor;

	}
	dev_info(mcu_app_dev->mcu_spi_dev.this, "space cache with size %u\n", mcu_app_dev->space_buffer_size);

	/* Bind the event handler and disable all mcu interrupts */
	status = write_uint64(mcu_app_dev, MCU_APP_INTERRUPT_ENABLE, 0);
	if (status < 0) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "could not set 'space.interrupts_enable': %d\n", status);
		goto error_destroy_descriptor;
	}
	mcu_spi_register_event(&mcu_app_dev->mcu_spi_dev, &mcu_app_dev->mcu_event);

	/* All good */
	return 0;

error_destroy_descriptor:
	mcu_descriptor_put(mcu_app_dev->descriptor);

error_remove_reset_state_attr:
	mcu_spi_remove_attr(&mcu_app_dev->mcu_spi_dev, "mcu-reset-state");

error_unregister:
	mcu_spi_unregister(&mcu_app_dev->mcu_spi_dev);

error_free_mcu_app_dev:
	kfree(mcu_app_dev);

	/* Something bad */
	return status;
}

module_init(mcu_app_init);
module_exit(mcu_app_exit);

MODULE_DESCRIPTION("Shaper Tool MCU APP driver");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com");
MODULE_LICENSE("GPL");
