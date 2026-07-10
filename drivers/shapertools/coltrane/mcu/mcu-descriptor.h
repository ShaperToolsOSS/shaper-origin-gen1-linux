#ifndef MCU_DESCRIPTOR_H_
#define MCU_DESCRIPTOR_H_

#include <linux/mutex.h>

#include <linux/mcu-ioctl.h>

#define MCU_DESCRIPTOR_MAX_NAME_SIZE 64UL
#define MCU_DESCRIPTOR_LEN_MAGIC 0x1371

struct mcu_app_device;
struct space_descriptor;
struct field_descriptor;

struct field_descriptor
{
	struct space_descriptor *parent;
	bool mapped;

	char name[MCU_DESCRIPTOR_MAX_NAME_SIZE];
	enum mcu_desc_field_type type;
	size_t size;
	size_t offset;
};

struct space_descriptor
{
	struct mcu_descriptor *parent;
	void *buffer;
	struct mutex lock;

	char name[MCU_DESCRIPTOR_MAX_NAME_SIZE];
	unsigned int id;
	size_t size;
	int num_fields;
	struct field_descriptor fields[];
};

struct mcu_descriptor
{
	struct kref kref;
	struct mcu_app_device *owner;
	int num_spaces;
	struct space_descriptor *spaces[];
};

struct mcu_descriptor *mcu_descriptor_new(struct mcu_app_device *mcu_app_dev, const void *desc_buffer, size_t len);
struct mcu_descriptor *mcu_descriptor_get(struct mcu_descriptor *desc);
void mcu_descriptor_put(struct mcu_descriptor *desc);

struct space_descriptor *mcu_descriptor_foreach_space(struct mcu_descriptor *desc, int (*entry)(struct space_descriptor *space_desc, void *context), void *context);
struct field_descriptor *mcu_descriptor_foreach_field(struct space_descriptor *desc, int (*entry)(struct field_descriptor *field_desc, void *context), void *context);

struct space_descriptor *mcu_descriptor_find_space_by_name(struct mcu_descriptor *desc, const char *name);
struct space_descriptor *mcu_descriptor_find_space_by_id(struct mcu_descriptor *desc, unsigned int id);

struct field_descriptor *mcu_descriptor_find_field_by_name(struct space_descriptor *desc, const char *name);
struct field_descriptor *mcu_descriptor_find_field_by_offset(struct space_descriptor *desc, size_t offset);

#endif
