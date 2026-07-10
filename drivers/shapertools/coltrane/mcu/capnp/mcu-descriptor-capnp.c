#include "mcu-descriptor-capnp.h"
/* AUTO GENERATED - DO NOT EDIT */
#ifdef __GNUC__
# define capnp_unused __attribute__((unused))
# define capnp_use(x) (void) x;
#else
# define capnp_unused
# define capnp_use(x)
#endif

static const capn_text capn_val0 = {0,"",0};

SpaceField_ptr new_SpaceField(struct capn_segment *s) {
	SpaceField_ptr p;
	p.p = capn_new_struct(s, 16, 1);
	return p;
}
SpaceField_list new_SpaceField_list(struct capn_segment *s, int len) {
	SpaceField_list p;
	p.p = capn_new_list(s, len, 16, 1);
	return p;
}
void read_SpaceField(struct SpaceField *s capnp_unused, SpaceField_ptr p) {
	capn_resolve(&p.p);
	capnp_use(s);
	s->name = capn_get_text(p.p, 0, capn_val0);
	s->type = (enum SpaceFieldType)(int) capn_read16(p.p, 0);
	s->flags = capn_read8(p.p, 2);
	s->size = capn_read32(p.p, 4);
	s->offset = capn_read32(p.p, 8);
}
void write_SpaceField(const struct SpaceField *s capnp_unused, SpaceField_ptr p) {
	capn_resolve(&p.p);
	capnp_use(s);
	capn_set_text(p.p, 0, s->name);
	capn_write16(p.p, 0, (uint16_t) (s->type));
	capn_write8(p.p, 2, s->flags);
	capn_write32(p.p, 4, s->size);
	capn_write32(p.p, 8, s->offset);
}
void get_SpaceField(struct SpaceField *s, SpaceField_list l, int i) {
	SpaceField_ptr p;
	p.p = capn_getp(l.p, i, 0);
	read_SpaceField(s, p);
}
void set_SpaceField(const struct SpaceField *s, SpaceField_list l, int i) {
	SpaceField_ptr p;
	p.p = capn_getp(l.p, i, 0);
	write_SpaceField(s, p);
}

capn_text SpaceField_get_name(SpaceField_ptr p)
{
	capn_text name;
	name = capn_get_text(p.p, 0, capn_val0);
	return name;
}

enum SpaceFieldType SpaceField_get_type(SpaceField_ptr p)
{
	enum SpaceFieldType type;
	type = (enum SpaceFieldType)(int) capn_read16(p.p, 0);
	return type;
}

uint8_t SpaceField_get_flags(SpaceField_ptr p)
{
	uint8_t flags;
	flags = capn_read8(p.p, 2);
	return flags;
}

uint32_t SpaceField_get_size(SpaceField_ptr p)
{
	uint32_t size;
	size = capn_read32(p.p, 4);
	return size;
}

uint32_t SpaceField_get_offset(SpaceField_ptr p)
{
	uint32_t offset;
	offset = capn_read32(p.p, 8);
	return offset;
}

void SpaceField_set_name(SpaceField_ptr p, capn_text name)
{
	capn_set_text(p.p, 0, name);
}

void SpaceField_set_type(SpaceField_ptr p, enum SpaceFieldType type)
{
	capn_write16(p.p, 0, (uint16_t) (type));
}

void SpaceField_set_flags(SpaceField_ptr p, uint8_t flags)
{
	capn_write8(p.p, 2, flags);
}

void SpaceField_set_size(SpaceField_ptr p, uint32_t size)
{
	capn_write32(p.p, 4, size);
}

void SpaceField_set_offset(SpaceField_ptr p, uint32_t offset)
{
	capn_write32(p.p, 8, offset);
}

SpaceDescriptor_ptr new_SpaceDescriptor(struct capn_segment *s) {
	SpaceDescriptor_ptr p;
	p.p = capn_new_struct(s, 8, 2);
	return p;
}
SpaceDescriptor_list new_SpaceDescriptor_list(struct capn_segment *s, int len) {
	SpaceDescriptor_list p;
	p.p = capn_new_list(s, len, 8, 2);
	return p;
}
void read_SpaceDescriptor(struct SpaceDescriptor *s capnp_unused, SpaceDescriptor_ptr p) {
	capn_resolve(&p.p);
	capnp_use(s);
	s->name = capn_get_text(p.p, 0, capn_val0);
	s->id = capn_read16(p.p, 0);
	s->flags = capn_read8(p.p, 2);
	s->size = capn_read32(p.p, 4);
	s->fields.p = capn_getp(p.p, 1, 0);
}
void write_SpaceDescriptor(const struct SpaceDescriptor *s capnp_unused, SpaceDescriptor_ptr p) {
	capn_resolve(&p.p);
	capnp_use(s);
	capn_set_text(p.p, 0, s->name);
	capn_write16(p.p, 0, s->id);
	capn_write8(p.p, 2, s->flags);
	capn_write32(p.p, 4, s->size);
	capn_setp(p.p, 1, s->fields.p);
}
void get_SpaceDescriptor(struct SpaceDescriptor *s, SpaceDescriptor_list l, int i) {
	SpaceDescriptor_ptr p;
	p.p = capn_getp(l.p, i, 0);
	read_SpaceDescriptor(s, p);
}
void set_SpaceDescriptor(const struct SpaceDescriptor *s, SpaceDescriptor_list l, int i) {
	SpaceDescriptor_ptr p;
	p.p = capn_getp(l.p, i, 0);
	write_SpaceDescriptor(s, p);
}

capn_text SpaceDescriptor_get_name(SpaceDescriptor_ptr p)
{
	capn_text name;
	name = capn_get_text(p.p, 0, capn_val0);
	return name;
}

uint16_t SpaceDescriptor_get_id(SpaceDescriptor_ptr p)
{
	uint16_t id;
	id = capn_read16(p.p, 0);
	return id;
}

uint8_t SpaceDescriptor_get_flags(SpaceDescriptor_ptr p)
{
	uint8_t flags;
	flags = capn_read8(p.p, 2);
	return flags;
}

uint32_t SpaceDescriptor_get_size(SpaceDescriptor_ptr p)
{
	uint32_t size;
	size = capn_read32(p.p, 4);
	return size;
}

SpaceField_list SpaceDescriptor_get_fields(SpaceDescriptor_ptr p)
{
	SpaceField_list fields;
	fields.p = capn_getp(p.p, 1, 0);
	return fields;
}

void SpaceDescriptor_set_name(SpaceDescriptor_ptr p, capn_text name)
{
	capn_set_text(p.p, 0, name);
}

void SpaceDescriptor_set_id(SpaceDescriptor_ptr p, uint16_t id)
{
	capn_write16(p.p, 0, id);
}

void SpaceDescriptor_set_flags(SpaceDescriptor_ptr p, uint8_t flags)
{
	capn_write8(p.p, 2, flags);
}

void SpaceDescriptor_set_size(SpaceDescriptor_ptr p, uint32_t size)
{
	capn_write32(p.p, 4, size);
}

void SpaceDescriptor_set_fields(SpaceDescriptor_ptr p, SpaceField_list fields)
{
	capn_setp(p.p, 1, fields.p);
}

McuDescriptors_ptr new_McuDescriptors(struct capn_segment *s) {
	McuDescriptors_ptr p;
	p.p = capn_new_struct(s, 0, 1);
	return p;
}
McuDescriptors_list new_McuDescriptors_list(struct capn_segment *s, int len) {
	McuDescriptors_list p;
	p.p = capn_new_list(s, len, 0, 1);
	return p;
}
void read_McuDescriptors(struct McuDescriptors *s capnp_unused, McuDescriptors_ptr p) {
	capn_resolve(&p.p);
	capnp_use(s);
	s->spaces.p = capn_getp(p.p, 0, 0);
}
void write_McuDescriptors(const struct McuDescriptors *s capnp_unused, McuDescriptors_ptr p) {
	capn_resolve(&p.p);
	capnp_use(s);
	capn_setp(p.p, 0, s->spaces.p);
}
void get_McuDescriptors(struct McuDescriptors *s, McuDescriptors_list l, int i) {
	McuDescriptors_ptr p;
	p.p = capn_getp(l.p, i, 0);
	read_McuDescriptors(s, p);
}
void set_McuDescriptors(const struct McuDescriptors *s, McuDescriptors_list l, int i) {
	McuDescriptors_ptr p;
	p.p = capn_getp(l.p, i, 0);
	write_McuDescriptors(s, p);
}

SpaceDescriptor_list McuDescriptors_get_spaces(McuDescriptors_ptr p)
{
	SpaceDescriptor_list spaces;
	spaces.p = capn_getp(p.p, 0, 0);
	return spaces;
}

void McuDescriptors_set_spaces(McuDescriptors_ptr p, SpaceDescriptor_list spaces)
{
	capn_setp(p.p, 0, spaces.p);
}
