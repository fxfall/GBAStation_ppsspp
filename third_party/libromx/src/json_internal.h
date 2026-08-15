#ifndef ROMX_JSON_INTERNAL_H
#define ROMX_JSON_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum romx_json_type {
    ROMX_JSON_OBJECT,
    ROMX_JSON_ARRAY,
    ROMX_JSON_STRING,
    ROMX_JSON_NUMBER,
    ROMX_JSON_TRUE,
    ROMX_JSON_FALSE,
    ROMX_JSON_NULL
} romx_json_type_t;

typedef struct romx_json_token {
    romx_json_type_t type;
    size_t start;
    size_t end;
    int parent;
    size_t child_count;
} romx_json_token_t;

typedef struct romx_json_document {
    const uint8_t *bytes;
    size_t size;
    romx_json_token_t *tokens;
    size_t token_count;
    size_t token_capacity;
} romx_json_document_t;

int romx_utf8_validate(const uint8_t *bytes, size_t size, size_t *bad_offset);
int romx_json_parse(romx_json_document_t *document, const uint8_t *bytes,
    size_t size, size_t *bad_offset);
void romx_json_destroy(romx_json_document_t *document);
int romx_json_object_value(const romx_json_document_t *document,
    int object_index, const char *key);
int romx_json_string_equals(const romx_json_document_t *document,
    int token_index, const char *value);
int romx_json_strings_equal(const romx_json_document_t *document,
    int first_token, int second_token);
int romx_json_copy_string(const romx_json_document_t *document,
    int token_index, char *buffer, size_t capacity, size_t *required);
size_t romx_json_string_characters(const romx_json_document_t *document,
    int token_index);
int romx_json_integer(const romx_json_document_t *document,
    int token_index, int64_t *value);
int romx_json_next_direct_child(const romx_json_document_t *document,
    int parent, int after);

#endif
