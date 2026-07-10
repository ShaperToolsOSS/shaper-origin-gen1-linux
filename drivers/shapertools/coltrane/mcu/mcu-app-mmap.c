#include <linux/mm.h>

#include "mcu-app.h"
#include "mcu-descriptor.h"

static vm_fault_t mcu_app_vm_fault(struct vm_fault *vmf)
{
	struct page *page;
	struct space_map *mapping = vmf->vma->vm_private_data;

	/* Bad shit */
	if (!mapping || !mapping->buffer)
		return VM_FAULT_SIGBUS;

	/* Set it up */
	page = virt_to_page(mapping->buffer);
	get_page(page);
	vmf->page = page;

	/* All good */
	return 0;
}

static void mcu_app_vm_open(struct vm_area_struct *vma)
{
	struct space_map *mapping = vma->vm_private_data;

	/* Track the number of users */
	++mapping->num_mappings;
}

static void mcu_app_vm_close(struct vm_area_struct *vma)
{
	struct space_map *mapping = vma->vm_private_data;
	void * buffer = mapping->buffer;

	/* Manage the buffer */
	if (--mapping->num_mappings == 0) {
		mapping->buffer = 0;
		free_page((unsigned long)buffer);
	}
}

static struct vm_operations_struct mcu_app_vm_ops =
{
	.open = mcu_app_vm_open,
	.close = mcu_app_vm_close,
	.fault = mcu_app_vm_fault,
};

int mcu_app_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct space_descriptor *desc;
	struct space_map *mapping;
	struct mcu_app_file *mcu_file = file->private_data;
	unsigned int space_id =  MCU_APP_TO_SPACE_NUM(vma->vm_pgoff * PAGE_SIZE);

	/* range check the space id */
	if (space_id >= MCU_APP_MAX_SPACES)
		return -EINVAL;

	/* Try to lookup the descriptor */
	desc = mcu_descriptor_find_space_by_id(mcu_file->mcu_app_dev->descriptor, space_id);
	if (!desc)
		return -EINVAL;

	/* Used the device mapping if sharing is requested */
	if ((vma->vm_flags & VM_SHARED) != 0)
		mapping = &mcu_file->mcu_app_dev->space_maps[space_id];
	else
		mapping = &mcu_file->space_maps[space_id];

	/* Do we need a buffer? */
	mutex_lock(&mcu_file->mcu_app_dev->space_maps_lock);
	if (!mapping->buffer)
		mapping->buffer = (void *)get_zeroed_page(GFP_KERNEL);
	mutex_unlock(&mcu_file->mcu_app_dev->space_maps_lock);

	/* Did we get do we buffer? */
	if (!mapping->buffer)
		return -ENOMEM;

	/* Everything is ready to go */
	vma->vm_ops = &mcu_app_vm_ops;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vma->vm_private_data = mapping;

	/* Empty for now, launch an async read? */
	mcu_app_vm_open(vma);

	/* All good */
	return 0;
}

int mcu_app_mmap_capture(struct file *file, unsigned int flags, unsigned int space_id, size_t offset, size_t len, struct timespec64 *timestamp)
{
	struct mcu_app_file *mcu_file = file->private_data;
	struct space_map *mapping = 0;

	/* range check the space id */
	if (space_id >= MCU_APP_MAX_SPACES)
		return -EINVAL;

	/* Extract the buffer */
	if (flags & MCU_BUFFER_GLOBAL)
		mapping = &mcu_file->mcu_app_dev->space_maps[space_id];
	else if (flags & MCU_BUFFER_LOCAL)
		mapping = &mcu_file->space_maps[space_id];

	/* Ensure the buffer has been mapped */
	if (!mapping || !mapping->buffer)
		return -EIO;

	/* Trim length to valid range */
	if (len == 0 || len > mapping->desc->size)
		len = mapping->desc->size;

	/* Range check the offset */
	if (offset + len > mapping->desc->size)
		return -EFBIG;

	/* Run it */
	return mcu_app_capture(mcu_file->mcu_app_dev, space_id, offset, mapping->buffer, len, timestamp);
}

int mcu_app_mmap_flush(struct file *file, unsigned int flags, unsigned int space_id, size_t offset, size_t len, struct timespec64 *timestamp)
{
	struct mcu_app_file *mcu_file = file->private_data;
	struct space_map *mapping = 0;

	/* range check the space id */
	if (space_id >= MCU_APP_MAX_SPACES)
		return -EINVAL;

	/* Extract the buffer */
	if (flags & MCU_BUFFER_GLOBAL)
		mapping = &mcu_file->mcu_app_dev->space_maps[space_id];
	else if (flags & MCU_BUFFER_LOCAL)
		mapping = &mcu_file->space_maps[space_id];

	/* Ensure the buffer has been mapped */
	if (!mapping || !mapping->buffer)
		return -EIO;

	/* Trim length to valid range */
	if (len == 0 || len > mapping->desc->size)
		len = mapping->desc->size;

	/* Range check the offset */
	if (offset + len > mapping->desc->size)
		return -EFBIG;

	/* Run it */
	return mcu_app_flush(mcu_file->mcu_app_dev, space_id, offset, mapping->buffer, len, timestamp);
}
