#include "mcu-app.h"

static void mcu_api_async_done(struct mcu_spi_transaction *trans, size_t count, int status)
{
	struct completion *completion = trans->context;

	/* Release the waiter */
	complete(completion);
}

int mcu_app_capture(struct mcu_app_device *mcu_app_dev, unsigned int space, size_t offset, void *buffer, size_t len, struct timespec64 *timestamp)
{
	int status;
	struct mcu_spi_transaction transaction;
	struct mcu_app_cmd_block mcu_cmd = { .flags = CMD_READ, .space = space, .size = len & 0xffff, .addr = offset };
	DECLARE_COMPLETION_ONSTACK(msg_complete);

	/* Initialize the transaction */
	mcu_spi_trans_init(&transaction, &mcu_cmd, sizeof(mcu_cmd), buffer, len, mcu_api_async_done);
	transaction.context = &msg_complete;
	transaction.xfers[0].rx_buf = 0;
	transaction.xfers[1].tx_buf = 0;

	/* Let it loose */
	status = mcu_spi_async(&mcu_app_dev->mcu_spi_dev, &transaction);
	if (status < 0) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed launching read transaction %u bytes from space %u at offset 0x%08x: %d\n", len, space, offset, status);
		return status;
	}

	/* Wait for the message and checkout completion status */
	status = wait_for_completion_timeout(&msg_complete, 5 * HZ);
	if (status < 0)
		panic("mcu_spi_capture transaction timed out after %d jiffies", 5 * HZ);

	/* Copy the start timestamp */
	if (timestamp)
		*timestamp = transaction.start;

	/* Log warning on transaction failure */
	if (transaction.msg.status < 0) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed reading %u bytes from space %u at offset 0x%08x: %d\n", len, space, offset, status);
		mcu_spi_send_reset(&mcu_app_dev->mcu_spi_dev);
	}

	/* Return the status */
	return transaction.msg.status < 0 ? transaction.msg.status : len;
}

int mcu_app_flush(struct mcu_app_device *mcu_app_dev, unsigned int space, size_t offset, const void *buffer, size_t len, struct timespec64 *timestamp)
{
	int status;
	struct mcu_spi_transaction transaction;
	struct mcu_app_cmd_block mcu_cmd = { .flags = CMD_WRITE, .space = space, .size = len & 0xffff, .addr = offset };
	DECLARE_COMPLETION_ONSTACK(msg_complete);

	/* Initialize the transaction */
	mcu_spi_trans_init(&transaction, &mcu_cmd, sizeof(mcu_cmd), (void *)buffer, len, mcu_api_async_done);
	transaction.context = &msg_complete;
	transaction.xfers[0].rx_buf = 0;
	transaction.xfers[1].rx_buf = 0;

	/* Let it loose */
	status = mcu_spi_async(&mcu_app_dev->mcu_spi_dev, &transaction);
	if (status < 0) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed launching write transaction %u bytes from space %u at offset 0x%08x: %d\n", len, space, offset, status);
		return status;
	}

	/* Wait for the message and checkout completion status */
	status = wait_for_completion_timeout(&msg_complete, 5 * HZ);
	if (status < 0)
		panic("mcu_spi_flush transaction timed out after %d jiffies", 5 * HZ);

	/* Copy the start timestamp */
	if (timestamp)
		*timestamp = transaction.start;

	/* Log warning on transaction failure */
	if (transaction.msg.status < 0) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed writing %u bytes from space %u at offset 0x%08x: %d\n", len, space, offset, status);
		mcu_spi_send_reset(&mcu_app_dev->mcu_spi_dev);
	}

	/* Return the status */
	return transaction.msg.status < 0 ? transaction.msg.status : len;
}

