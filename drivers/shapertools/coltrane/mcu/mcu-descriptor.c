/*#define DEBUG*/

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/sort.h>
#include <linux/bsearch.h>

#include "mcu-app.h"
#include "mcu-descriptor.h"
#include "capnp/mcu-descriptor-capnp.h"
#include "capnp/capnp_c.h"

static const char *const type_str[] =
{
	/* Base types */
	"float",
	"double",
	"char",
	"int8_t",
	"uint8_t",
	"int16_t",
	"uint16_t",
	"int32_t",
	"uint32_t",
	"int64_t",
	"uint64_t",

	/* Array Types */
	"float",
	"double",
	"char",
	"int8_t",
	"uint8_t",
	"int16_t",
	"uint16_t",
	"int32_t",
	"uint32_t",
	"int64_t",
	"uint64_t",

	/* Triple types */
	"float_triple_t",
	"double_triple_t",
	"int8_triple_t",
	"uint8_triple_t",
	"int16_triple_t",
	"uint16_triple_t",
	"int32_triple_t",
	"uint32_triple_t",
	"int64_triple_t",
	"uint64_triple_t",
};

static ssize_t show_descriptor(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mcu_spi_device_attribute *mcu_spi_device_attr = to_mcu_spi_device_attr(attr);
	struct space_descriptor *desc = mcu_spi_device_attr->context;
	const char *type_name;
	int status = 0;
	int i;

	/* Dump the descriptor */
	status = scnprintf(buf, PAGE_SIZE, "%s: type: %u size: %u\n", desc->name, desc->id, desc->size);
	for (i = 0; i < desc->num_fields; ++i) {

		/* Range check the type */
		type_name = "unknown";
		if (desc->fields[i].type < ARRAY_SIZE(type_str))
			type_name = type_str[desc->fields[i].type];

		/* Display array type with size */
		if (desc->fields[i].type >= MCU_DESC_FIELD_TYPE_FLOAT_ARRAY && desc->fields[i].type <= MCU_DESC_FIELD_TYPE_UINT64_ARRAY)
			status += scnprintf(buf + status, PAGE_SIZE - status, "\t0x%08x: size: %4u %s %s[]\n", desc->fields[i].offset, desc->fields[i].size, type_name, desc->fields[i].name);
		else
			status += scnprintf(buf + status, PAGE_SIZE - status, "\t0x%08x: size: %4u %s %s\n", desc->fields[i].offset, desc->fields[i].size, type_name, desc->fields[i].name);
	}

	/* Success */
	return status;
}

static ssize_t show_descriptor_list(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mcu_spi_device_attribute *mcu_spi_device_attr = to_mcu_spi_device_attr(attr);
	struct mcu_descriptor *desc = mcu_spi_device_attr->context;
	int status = 0;
	int i;

	/* Dump the descriptor entry */
	for (i = 0; i < desc->num_spaces; ++i)
		status += scnprintf(buf + status, PAGE_SIZE - status, "%2u status: %1lld name: %s\n", desc->spaces[i]->id, (desc->owner->mcu_status.space_active >> desc->spaces[i]->id) & 0x01, desc->spaces[i]->name);

	/* Do at last */
	return status;
}

static int compare_space_id(const void *first, const void *second)
{
	const struct space_descriptor *first_desc = *(const struct space_descriptor **)first;
	const struct space_descriptor *second_desc = *(const struct space_descriptor **)second;

	if (first_desc->id < second_desc->id)
		return -1;
	else if (first_desc->id > second_desc->id)
		return 1;
	return 0;
}

static void mcu_descriptor_release(struct kref *kref)
{
	int s;
	int status;
	struct mcu_descriptor *desc = container_of(kref, struct mcu_descriptor, kref);
	char sysfs_name[64];

	if (!desc)
		return;

	dev_dbg(desc->owner->mcu_spi_dev.this, "releasing descriptor @ %p\n", desc);

	/* Loop though the spaces */
	for (s = 0; s < desc->num_spaces; ++s) {

		/* Always unmap the space desc */
		scnprintf(sysfs_name, sizeof(sysfs_name), "%s-desc", desc->spaces[s]->name);
		strreplace(sysfs_name, '_', '-');
		status = mcu_spi_remove_attr(&desc->owner->mcu_spi_dev, sysfs_name);
		if (status != 0)
			dev_warn(desc->owner->mcu_spi_dev.this, "failed to unmap sysfs attr descriptor %s: %d\n", sysfs_name, status);

		/* Clean up the remaining space memory */
		kfree(desc->spaces[s]);
	}

	/* Unmap the descriptor list */
	status = mcu_spi_remove_attr(&desc->owner->mcu_spi_dev, "desc-list");
	if (status < 0)
		dev_warn(desc->owner->mcu_spi_dev.this, "could not unmap desc-list: %d\n", status);

	/* Let go of the descriptor itself */
	kfree(desc);
}

struct mcu_descriptor *mcu_descriptor_new(struct mcu_app_device *mcu_app_dev, const void *desc_buffer, size_t len)
{
	struct capn arena;
	McuDescriptors_ptr mcu_descs;
	SpaceDescriptor_ptr space;
	SpaceDescriptor_list spaces;
	SpaceField_ptr field;
	SpaceField_list fields;
	int s;
	int f;
	struct mcu_descriptor *desc;
	int status;
	char sysfs_name[64];

	dev_info(mcu_app_dev->mcu_spi_dev.this, "mcu descriptor new enter\n");
	/* Initialize the arena for the descriptor */
	if (capn_init_mem(&arena, desc_buffer, len, 1) < 0) {
		dev_err(mcu_app_dev->mcu_spi_dev.this, "%i problem initializing the descriptor arena\n", __LINE__);
		return 0;
	}

	/* Extract the root descriptor */
	mcu_descs.p = capn_getp(capn_root(&arena), 0, 0);
	if (mcu_descs.p.type == CAPN_NULL) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "%i problem initializing the descriptor arena\n", __LINE__);
		goto error_free_capn;
	}

	/* Extract the space pointer */
	spaces = McuDescriptors_get_spaces(mcu_descs);
	if (spaces.p.type == CAPN_NULL) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem spaces list is null\n");
		goto error_free_capn;
	}

	/* Allocate the descriptor with enough room for pointer to each space descriptor */
	desc = kzalloc(sizeof(struct mcu_descriptor) + capn_len(spaces) * sizeof(struct space_descriptor *), GFP_KERNEL);
	if (!desc) {
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem allocated mcu descriptor with %u space slots\n", capn_len(spaces));
		return 0;
	}
	desc->owner = mcu_app_dev;
	desc->num_spaces = capn_len(spaces);
	kref_init(&desc->kref);

	/* Loop through each space */
	for (s = 0; s < desc->num_spaces; ++s) {

		/* Extract the current space */
		space.p = capn_getp(spaces.p, s, 1);
		if (space.p.type == CAPN_NULL) {
			dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem extracting space %d\n", s);
			goto error_desc_destroy;
		}

		/* Extract the fields */
		fields = SpaceDescriptor_get_fields(space);

		/* Allocate the space descriptor */
		desc->spaces[s] = kzalloc(sizeof(struct space_descriptor) + (capn_len(fields) * sizeof(struct field_descriptor)), GFP_KERNEL);
		if (!desc->spaces[s]) {
			dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem allocated mcu space descriptor with %u field slots\n", capn_len(fields));
			goto error_desc_destroy;
		}

		/* Initialize the space */
		desc->spaces[s]->parent = desc;
		desc->spaces[s]->id = SpaceDescriptor_get_id(space);
		desc->spaces[s]->size = SpaceDescriptor_get_size(space);
		desc->spaces[s]->num_fields = capn_len(fields);
		scnprintf(desc->spaces[s]->name, sizeof(desc->spaces[s]->name), "%s", SpaceDescriptor_get_name(space).str);

		/* Loop through each field creating */
		for (f = 0; f < desc->spaces[s]->num_fields; ++f) {

			/* Extract the current field */
			field.p = capn_getp(fields.p, f, 1);
			if (field.p.type == CAPN_NULL) {
				dev_warn(mcu_app_dev->mcu_spi_dev.this, "problem extracting field %d\n", f);
				goto error_desc_destroy;
			}

			/* Initialize the field */
			desc->spaces[s]->fields[f].parent = desc->spaces[s];
			desc->spaces[s]->fields[f].type = SpaceField_get_type(field);
			desc->spaces[s]->fields[f].size = SpaceField_get_size(field);
			desc->spaces[s]->fields[f].offset = SpaceField_get_offset(field);
			scnprintf(desc->spaces[s]->fields[f].name, sizeof(desc->spaces[s]->fields[f].name), "%s", SpaceField_get_name(field).str);
		}

		/* Map the space descriptor attr */
		scnprintf(sysfs_name, sizeof(sysfs_name), "%s-desc", desc->spaces[s]->name);
		strreplace(sysfs_name, '_', '-');
		status = mcu_spi_add_attr(&mcu_app_dev->mcu_spi_dev, sysfs_name, show_descriptor, 0, desc->spaces[s]);
		if (status != 0)
			dev_warn(mcu_app_dev->mcu_spi_dev.this, "failed to map sysfs attr descriptor %s: %d\n", sysfs_name, status);
		dev_dbg(mcu_app_dev->mcu_spi_dev.this, "mapped %s\n", sysfs_name);
	}

	/* Release the arena */
	capn_free(&arena);

	/* Sort the space descriptors */
	sort(desc->spaces, desc->num_spaces, sizeof(struct space_descriptor *), compare_space_id, 0);

	/* Map the descriptor list sysfs attr */
	status = mcu_spi_add_attr(&mcu_app_dev->mcu_spi_dev, "desc-list", show_descriptor_list, 0, desc);
	if (status < 0)
		dev_warn(mcu_app_dev->mcu_spi_dev.this, "could not map descriptor list: %d\n", status);

	/* Return the new descriptor */
	return desc;

error_desc_destroy:
	mcu_descriptor_put(desc);

error_free_capn:
	/* Release the arena */
	capn_free(&arena);

	return 0;
}

struct mcu_descriptor *mcu_descriptor_get(struct mcu_descriptor *desc)
{
	if (desc)
		kref_get(&desc->kref);

	dev_dbg(desc->owner->mcu_spi_dev.this, "getting descriptor @ %p ref: %ld\n", desc, atomic_long_read(&desc->kref.refcount.refs));

	return desc;
}

void mcu_descriptor_put(struct mcu_descriptor *desc)
{
	if (desc) {
		dev_dbg(desc->owner->mcu_spi_dev.this, "putting descriptor @ %p ref: %ld\n", desc, atomic_long_read(&desc->kref.refcount.refs));
		kref_put(&desc->kref, mcu_descriptor_release);
	}
}

struct space_descriptor *mcu_descriptor_foreach_space(struct mcu_descriptor *desc, int (*entry)(struct space_descriptor *space_desc, void *context), void *context)
{
	int i;
	for (i = 0; i < desc->num_spaces; ++i)
		if (entry(desc->spaces[i], context))
			return desc->spaces[i];
	return 0;
}

struct field_descriptor *mcu_descriptor_foreach_field(struct space_descriptor *desc, int (*entry)(struct field_descriptor *field_desc, void *context), void *context)
{
	int i;
	for (i = 0; i < desc->num_fields; ++i)
		if (entry(&desc->fields[i], context))
			return  &desc->fields[i];
	return 0;
}

static int match_space_name(struct space_descriptor *space_desc, void *context)
{
	const char *name = context;
	return strcmp(name, space_desc->name) == 0;
}

static int match_space_id(const void *key, const void *element)
{
	unsigned int space_key = *(unsigned int *)key;
	const struct space_descriptor *desc = *(const struct space_descriptor **)element;

	if (space_key < desc->id)
		return -1;
	else if (space_key > desc->id)
		return 1;
	return 0;
}

struct space_descriptor *mcu_descriptor_find_space_by_name(struct mcu_descriptor *desc, const char *name)
{
	if (!desc)
		return 0;

	return mcu_descriptor_foreach_space(desc, match_space_name, (void *)name);
}

struct space_descriptor *mcu_descriptor_find_space_by_id(struct mcu_descriptor *desc, unsigned int id)
{
	struct space_descriptor **space_desc = 0;

	if (!desc)
		return 0;

	space_desc = (struct space_descriptor **)bsearch(&id, desc->spaces, desc->num_spaces, sizeof(struct space_descriptor *), match_space_id);
	if (!space_desc)
		return 0;
	return *space_desc;
}

static int match_field_name(struct field_descriptor *field_desc, void *context)
{
	const char *name = context;
	return strcmp(name, field_desc->name) == 0;
}

static int match_field_offset(struct field_descriptor *field_desc, void *context)
{
	size_t offset = (size_t)context;
	return offset == field_desc->offset;
}

struct field_descriptor *mcu_descriptor_find_field_by_name(struct space_descriptor *desc, const char *name)
{
	if (!desc)
		return 0;
	return mcu_descriptor_foreach_field(desc, match_field_name, (void *)name);
}

struct field_descriptor *mcu_descriptor_find_space_by_offset(struct space_descriptor *desc, size_t offset)
{
	if (!desc)
		return 0;
	return mcu_descriptor_foreach_field(desc, match_field_offset, (void *)offset);
}
