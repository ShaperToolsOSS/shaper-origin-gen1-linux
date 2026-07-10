#ifndef CAPN_8A0824CAFE8F4D4C
#define CAPN_8A0824CAFE8F4D4C
/* AUTO GENERATED - DO NOT EDIT */
#include "capnp_c.h"

#if CAPN_VERSION != 1
#error "version mismatch between capnp_c.h and generated code"
#endif

#ifndef capnp_nowarn
# ifdef __GNUC__
#  define capnp_nowarn __extension__
# else
#  define capnp_nowarn
# endif
#endif

/*#include "c.capnp.h"*/

#ifdef __cplusplus
extern "C" {
#endif

struct SpaceField;
struct SpaceDescriptor;
struct McuDescriptors;

typedef struct {capn_ptr p;} SpaceField_ptr;
typedef struct {capn_ptr p;} SpaceDescriptor_ptr;
typedef struct {capn_ptr p;} McuDescriptors_ptr;

typedef struct {capn_ptr p;} SpaceField_list;
typedef struct {capn_ptr p;} SpaceDescriptor_list;
typedef struct {capn_ptr p;} McuDescriptors_list;

enum SpaceFieldType {
	SpaceFieldType_float = 0,
	SpaceFieldType_double = 1,
	SpaceFieldType_char = 2,
	SpaceFieldType_int8 = 3,
	SpaceFieldType_uint8 = 4,
	SpaceFieldType_int16 = 5,
	SpaceFieldType_uint16 = 6,
	SpaceFieldType_int32 = 7,
	SpaceFieldType_uint32 = 8,
	SpaceFieldType_int64 = 9,
	SpaceFieldType_uint64 = 10,
	SpaceFieldType_floatArray = 11,
	SpaceFieldType_doubleArray = 12,
	SpaceFieldType_charArray = 13,
	SpaceFieldType_int8Array = 14,
	SpaceFieldType_uint8Array = 15,
	SpaceFieldType_int16Array = 16,
	SpaceFieldType_uint16Array = 17,
	SpaceFieldType_int32Array = 18,
	SpaceFieldType_uint32Array = 19,
	SpaceFieldType_int64Array = 20,
	SpaceFieldType_uint64Array = 21,
	SpaceFieldType_floatTriple = 22,
	SpaceFieldType_doubleTriple = 23,
	SpaceFieldType_int8Triple = 24,
	SpaceFieldType_uint8Triple = 25,
	SpaceFieldType_int16Triple = 26,
	SpaceFieldType_uint16Triple = 27,
	SpaceFieldType_int32Triple = 28,
	SpaceFieldType_uint32Triple = 29,
	SpaceFieldType_int64Triple = 30,
	SpaceFieldType_uint64Triple = 31
};

struct SpaceField {
	capn_text name;
	enum SpaceFieldType type;
	uint8_t flags;
	uint32_t size;
	uint32_t offset;
};

static const size_t SpaceField_word_count = 2;

static const size_t SpaceField_pointer_count = 1;

static const size_t SpaceField_struct_bytes_count = 24;

capn_text SpaceField_get_name(SpaceField_ptr p);

enum SpaceFieldType SpaceField_get_type(SpaceField_ptr p);

uint8_t SpaceField_get_flags(SpaceField_ptr p);

uint32_t SpaceField_get_size(SpaceField_ptr p);

uint32_t SpaceField_get_offset(SpaceField_ptr p);

void SpaceField_set_name(SpaceField_ptr p, capn_text name);

void SpaceField_set_type(SpaceField_ptr p, enum SpaceFieldType type);

void SpaceField_set_flags(SpaceField_ptr p, uint8_t flags);

void SpaceField_set_size(SpaceField_ptr p, uint32_t size);

void SpaceField_set_offset(SpaceField_ptr p, uint32_t offset);

struct SpaceDescriptor {
	capn_text name;
	uint16_t id;
	uint8_t flags;
	uint32_t size;
	SpaceField_list fields;
};

static const size_t SpaceDescriptor_word_count = 1;

static const size_t SpaceDescriptor_pointer_count = 2;

static const size_t SpaceDescriptor_struct_bytes_count = 24;

capn_text SpaceDescriptor_get_name(SpaceDescriptor_ptr p);

uint16_t SpaceDescriptor_get_id(SpaceDescriptor_ptr p);

uint8_t SpaceDescriptor_get_flags(SpaceDescriptor_ptr p);

uint32_t SpaceDescriptor_get_size(SpaceDescriptor_ptr p);

SpaceField_list SpaceDescriptor_get_fields(SpaceDescriptor_ptr p);

void SpaceDescriptor_set_name(SpaceDescriptor_ptr p, capn_text name);

void SpaceDescriptor_set_id(SpaceDescriptor_ptr p, uint16_t id);

void SpaceDescriptor_set_flags(SpaceDescriptor_ptr p, uint8_t flags);

void SpaceDescriptor_set_size(SpaceDescriptor_ptr p, uint32_t size);

void SpaceDescriptor_set_fields(SpaceDescriptor_ptr p, SpaceField_list fields);

struct McuDescriptors {
	SpaceDescriptor_list spaces;
};

static const size_t McuDescriptors_word_count = 0;

static const size_t McuDescriptors_pointer_count = 1;

static const size_t McuDescriptors_struct_bytes_count = 8;

SpaceDescriptor_list McuDescriptors_get_spaces(McuDescriptors_ptr p);

void McuDescriptors_set_spaces(McuDescriptors_ptr p, SpaceDescriptor_list spaces);

SpaceField_ptr new_SpaceField(struct capn_segment*);
SpaceDescriptor_ptr new_SpaceDescriptor(struct capn_segment*);
McuDescriptors_ptr new_McuDescriptors(struct capn_segment*);

SpaceField_list new_SpaceField_list(struct capn_segment*, int len);
SpaceDescriptor_list new_SpaceDescriptor_list(struct capn_segment*, int len);
McuDescriptors_list new_McuDescriptors_list(struct capn_segment*, int len);

void read_SpaceField(struct SpaceField*, SpaceField_ptr);
void read_SpaceDescriptor(struct SpaceDescriptor*, SpaceDescriptor_ptr);
void read_McuDescriptors(struct McuDescriptors*, McuDescriptors_ptr);

void write_SpaceField(const struct SpaceField*, SpaceField_ptr);
void write_SpaceDescriptor(const struct SpaceDescriptor*, SpaceDescriptor_ptr);
void write_McuDescriptors(const struct McuDescriptors*, McuDescriptors_ptr);

void get_SpaceField(struct SpaceField*, SpaceField_list, int i);
void get_SpaceDescriptor(struct SpaceDescriptor*, SpaceDescriptor_list, int i);
void get_McuDescriptors(struct McuDescriptors*, McuDescriptors_list, int i);

void set_SpaceField(const struct SpaceField*, SpaceField_list, int i);
void set_SpaceDescriptor(const struct SpaceDescriptor*, SpaceDescriptor_list, int i);
void set_McuDescriptors(const struct McuDescriptors*, McuDescriptors_list, int i);

#ifdef __cplusplus
}
#endif
#endif
