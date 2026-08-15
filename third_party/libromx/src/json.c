#include "json_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct romx_json_parser {
    romx_json_document_t *document;
    size_t position;
    size_t bad_offset;
    unsigned int depth;
} romx_json_parser_t;

static int hex_value(uint8_t byte)
{
    if (byte >= (uint8_t)'0' && byte <= (uint8_t)'9') return (int)(byte - (uint8_t)'0');
    if (byte >= (uint8_t)'a' && byte <= (uint8_t)'f') return (int)(byte - (uint8_t)'a') + 10;
    if (byte >= (uint8_t)'A' && byte <= (uint8_t)'F') return (int)(byte - (uint8_t)'A') + 10;
    return -1;
}

static int read_hex4(const uint8_t *bytes, size_t size, size_t position, uint32_t *value)
{
    unsigned int index;
    uint32_t result = UINT32_C(0);
    if (position > size || size - position < 4U) return 0;
    for (index = 0U; index < 4U; ++index) {
        const int digit = hex_value(bytes[position + index]);
        if (digit < 0) return 0;
        result = (result << 4) | (uint32_t)digit;
    }
    *value = result;
    return 1;
}

int romx_utf8_validate(const uint8_t *bytes, size_t size, size_t *bad_offset)
{
    size_t position = 0U;
    while (position < size) {
        const uint8_t first = bytes[position];
        size_t count;
        uint32_t value;
        uint32_t minimum;
        size_t index;
        if (first < UINT8_C(0x80)) { ++position; continue; }
        if (first >= UINT8_C(0xc2) && first <= UINT8_C(0xdf)) {
            count = 2U; value = first & UINT8_C(0x1f); minimum = UINT32_C(0x80);
        } else if (first >= UINT8_C(0xe0) && first <= UINT8_C(0xef)) {
            count = 3U; value = first & UINT8_C(0x0f); minimum = UINT32_C(0x800);
        } else if (first >= UINT8_C(0xf0) && first <= UINT8_C(0xf4)) {
            count = 4U; value = first & UINT8_C(0x07); minimum = UINT32_C(0x10000);
        } else {
            if (bad_offset != NULL) *bad_offset = position;
            return 0;
        }
        if (size - position < count) {
            if (bad_offset != NULL) *bad_offset = position;
            return 0;
        }
        for (index = 1U; index < count; ++index) {
            const uint8_t next = bytes[position + index];
            if ((next & UINT8_C(0xc0)) != UINT8_C(0x80)) {
                if (bad_offset != NULL) *bad_offset = position + index;
                return 0;
            }
            value = (value << 6) | (uint32_t)(next & UINT8_C(0x3f));
        }
        if (value < minimum || value > UINT32_C(0x10ffff) ||
            (value >= UINT32_C(0xd800) && value <= UINT32_C(0xdfff))) {
            if (bad_offset != NULL) *bad_offset = position;
            return 0;
        }
        position += count;
    }
    return 1;
}

static void skip_space(romx_json_parser_t *parser)
{
    const uint8_t *bytes = parser->document->bytes;
    while (parser->position < parser->document->size) {
        const uint8_t byte = bytes[parser->position];
        if (byte != (uint8_t)' ' && byte != (uint8_t)'\t' &&
            byte != (uint8_t)'\r' && byte != (uint8_t)'\n') break;
        ++parser->position;
    }
}

static int add_token(romx_json_parser_t *parser, romx_json_type_t type,
    size_t start, int parent)
{
    romx_json_document_t *document = parser->document;
    romx_json_token_t *tokens;
    size_t capacity;
    int index;
    if (document->token_count == document->token_capacity) {
        capacity = document->token_capacity == 0U ? 64U : document->token_capacity * 2U;
        if (capacity < document->token_capacity ||
            capacity > SIZE_MAX / sizeof(*document->tokens)) return -1;
        tokens = (romx_json_token_t *)realloc(document->tokens,
            capacity * sizeof(*document->tokens));
        if (tokens == NULL) return -1;
        document->tokens = tokens;
        document->token_capacity = capacity;
    }
    index = (int)document->token_count++;
    document->tokens[index].type = type;
    document->tokens[index].start = start;
    document->tokens[index].end = start;
    document->tokens[index].parent = parent;
    document->tokens[index].child_count = 0U;
    if (parent >= 0) ++document->tokens[parent].child_count;
    return index;
}

static int parse_string(romx_json_parser_t *parser, int parent)
{
    const uint8_t *bytes = parser->document->bytes;
    const size_t size = parser->document->size;
    const size_t start = parser->position;
    int token;
    ++parser->position;
    while (parser->position < size) {
        const uint8_t byte = bytes[parser->position++];
        if (byte == (uint8_t)'"') {
            token = add_token(parser, ROMX_JSON_STRING, start, parent);
            if (token < 0) return -1;
            parser->document->tokens[token].end = parser->position;
            return token;
        }
        if (byte < UINT8_C(0x20)) { parser->bad_offset = parser->position - 1U; return -1; }
        if (byte == (uint8_t)'\\') {
            uint8_t escape;
            if (parser->position >= size) { parser->bad_offset = parser->position; return -1; }
            escape = bytes[parser->position++];
            if (escape == (uint8_t)'u') {
                uint32_t first;
                if (!read_hex4(bytes, size, parser->position, &first)) {
                    parser->bad_offset = parser->position; return -1;
                }
                parser->position += 4U;
                if (first >= UINT32_C(0xd800) && first <= UINT32_C(0xdbff)) {
                    uint32_t second;
                    if (size - parser->position < 6U || bytes[parser->position] != (uint8_t)'\\' ||
                        bytes[parser->position + 1U] != (uint8_t)'u' ||
                        !read_hex4(bytes, size, parser->position + 2U, &second) ||
                        second < UINT32_C(0xdc00) || second > UINT32_C(0xdfff)) {
                        parser->bad_offset = parser->position; return -1;
                    }
                    parser->position += 6U;
                } else if (first >= UINT32_C(0xdc00) && first <= UINT32_C(0xdfff)) {
                    parser->bad_offset = parser->position - 4U; return -1;
                }
            } else if (escape != (uint8_t)'"' && escape != (uint8_t)'\\' &&
                escape != (uint8_t)'/' && escape != (uint8_t)'b' && escape != (uint8_t)'f' &&
                escape != (uint8_t)'n' && escape != (uint8_t)'r' && escape != (uint8_t)'t') {
                parser->bad_offset = parser->position - 1U; return -1;
            }
        }
    }
    parser->bad_offset = start;
    return -1;
}

static int parse_number(romx_json_parser_t *parser, int parent)
{
    const uint8_t *b = parser->document->bytes;
    const size_t size = parser->document->size;
    const size_t start = parser->position;
    int token;
    if (b[parser->position] == (uint8_t)'-') ++parser->position;
    if (parser->position >= size) goto invalid;
    if (b[parser->position] == (uint8_t)'0') {
        ++parser->position;
        if (parser->position < size && b[parser->position] >= (uint8_t)'0' &&
            b[parser->position] <= (uint8_t)'9') goto invalid;
    } else if (b[parser->position] >= (uint8_t)'1' && b[parser->position] <= (uint8_t)'9') {
        do { ++parser->position; } while (parser->position < size &&
            b[parser->position] >= (uint8_t)'0' && b[parser->position] <= (uint8_t)'9');
    } else goto invalid;
    if (parser->position < size && b[parser->position] == (uint8_t)'.') {
        ++parser->position;
        if (parser->position >= size || b[parser->position] < (uint8_t)'0' ||
            b[parser->position] > (uint8_t)'9') goto invalid;
        do { ++parser->position; } while (parser->position < size &&
            b[parser->position] >= (uint8_t)'0' && b[parser->position] <= (uint8_t)'9');
    }
    if (parser->position < size && (b[parser->position] == (uint8_t)'e' ||
        b[parser->position] == (uint8_t)'E')) {
        ++parser->position;
        if (parser->position < size && (b[parser->position] == (uint8_t)'+' ||
            b[parser->position] == (uint8_t)'-')) ++parser->position;
        if (parser->position >= size || b[parser->position] < (uint8_t)'0' ||
            b[parser->position] > (uint8_t)'9') goto invalid;
        do { ++parser->position; } while (parser->position < size &&
            b[parser->position] >= (uint8_t)'0' && b[parser->position] <= (uint8_t)'9');
    }
    token = add_token(parser, ROMX_JSON_NUMBER, start, parent);
    if (token < 0) return -1;
    parser->document->tokens[token].end = parser->position;
    return token;
invalid:
    parser->bad_offset = parser->position;
    return -1;
}

static int parse_value(romx_json_parser_t *parser, int parent);

static int parse_container(romx_json_parser_t *parser, int parent, int object)
{
    const uint8_t close = object ? (uint8_t)'}' : (uint8_t)']';
    const size_t start = parser->position;
    int token;
    if (++parser->depth > 128U) { parser->bad_offset = start; return -1; }
    token = add_token(parser, object ? ROMX_JSON_OBJECT : ROMX_JSON_ARRAY, start, parent);
    if (token < 0) return -1;
    ++parser->position;
    skip_space(parser);
    if (parser->position < parser->document->size &&
        parser->document->bytes[parser->position] == close) {
        ++parser->position;
        parser->document->tokens[token].end = parser->position;
        --parser->depth;
        return token;
    }
    for (;;) {
        if (object) {
            if (parser->position >= parser->document->size ||
                parser->document->bytes[parser->position] != (uint8_t)'"' ||
                parse_string(parser, token) < 0) return -1;
            skip_space(parser);
            if (parser->position >= parser->document->size ||
                parser->document->bytes[parser->position++] != (uint8_t)':') {
                parser->bad_offset = parser->position; return -1;
            }
            skip_space(parser);
        }
        if (parse_value(parser, token) < 0) return -1;
        skip_space(parser);
        if (parser->position >= parser->document->size) {
            parser->bad_offset = parser->position; return -1;
        }
        if (parser->document->bytes[parser->position] == close) {
            ++parser->position;
            parser->document->tokens[token].end = parser->position;
            --parser->depth;
            return token;
        }
        if (parser->document->bytes[parser->position++] != (uint8_t)',') {
            parser->bad_offset = parser->position - 1U; return -1;
        }
        skip_space(parser);
        if (parser->position >= parser->document->size ||
            parser->document->bytes[parser->position] == close) {
            parser->bad_offset = parser->position; return -1;
        }
    }
}

static int parse_value(romx_json_parser_t *parser, int parent)
{
    const uint8_t *b = parser->document->bytes;
    const size_t size = parser->document->size;
    int token;
    skip_space(parser);
    if (parser->position >= size) { parser->bad_offset = parser->position; return -1; }
    if (b[parser->position] == (uint8_t)'{') return parse_container(parser, parent, 1);
    if (b[parser->position] == (uint8_t)'[') return parse_container(parser, parent, 0);
    if (b[parser->position] == (uint8_t)'"') return parse_string(parser, parent);
    if (b[parser->position] == (uint8_t)'-' ||
        (b[parser->position] >= (uint8_t)'0' && b[parser->position] <= (uint8_t)'9'))
        return parse_number(parser, parent);
#define ROMX_LITERAL(text, json_type) \
    if (size - parser->position >= sizeof(text) - 1U && \
        memcmp(b + parser->position, text, sizeof(text) - 1U) == 0) { \
        const size_t start = parser->position; \
        parser->position += sizeof(text) - 1U; \
        token = add_token(parser, json_type, start, parent); \
        if (token >= 0) parser->document->tokens[token].end = parser->position; \
        return token; \
    }
    ROMX_LITERAL("true", ROMX_JSON_TRUE)
    ROMX_LITERAL("false", ROMX_JSON_FALSE)
    ROMX_LITERAL("null", ROMX_JSON_NULL)
#undef ROMX_LITERAL
    parser->bad_offset = parser->position;
    return -1;
}

int romx_json_parse(romx_json_document_t *document, const uint8_t *bytes,
    size_t size, size_t *bad_offset)
{
    romx_json_parser_t parser;
    int root;
    memset(document, 0, sizeof(*document));
    document->bytes = bytes;
    document->size = size;
    memset(&parser, 0, sizeof(parser));
    parser.document = document;
    root = parse_value(&parser, -1);
    skip_space(&parser);
    if (root != 0 || parser.position != size) {
        if (bad_offset != NULL) *bad_offset = root < 0 ? parser.bad_offset : parser.position;
        romx_json_destroy(document);
        return 0;
    }
    return 1;
}

void romx_json_destroy(romx_json_document_t *document)
{
    free(document->tokens);
    memset(document, 0, sizeof(*document));
}

static int decode_next(const uint8_t *bytes, size_t end, size_t *position, uint32_t *value)
{
    uint8_t byte = bytes[(*position)++];
    if (byte != (uint8_t)'\\') { *value = byte; return 1; }
    byte = bytes[(*position)++];
    switch (byte) {
    case (uint8_t)'"': case (uint8_t)'\\': case (uint8_t)'/': *value = byte; return 1;
    case (uint8_t)'b': *value = UINT32_C(8); return 1;
    case (uint8_t)'f': *value = UINT32_C(12); return 1;
    case (uint8_t)'n': *value = UINT32_C(10); return 1;
    case (uint8_t)'r': *value = UINT32_C(13); return 1;
    case (uint8_t)'t': *value = UINT32_C(9); return 1;
    default:
        if (byte == (uint8_t)'u') {
            uint32_t first;
            if (!read_hex4(bytes, end, *position, &first)) return 0;
            *position += 4U;
            if (first >= UINT32_C(0xd800) && first <= UINT32_C(0xdbff)) {
                uint32_t second;
                *position += 2U;
                if (!read_hex4(bytes, end, *position, &second)) return 0;
                *position += 4U;
                first = UINT32_C(0x10000) + ((first - UINT32_C(0xd800)) << 10)
                    + (second - UINT32_C(0xdc00));
            }
            *value = first;
            return 1;
        }
        return 0;
    }
}

static size_t utf8_size(uint32_t value)
{
    if (value < UINT32_C(0x80)) return 1U;
    if (value < UINT32_C(0x800)) return 2U;
    if (value < UINT32_C(0x10000)) return 3U;
    return 4U;
}

static void write_utf8(char *buffer, size_t *position, uint32_t value)
{
    if (value < UINT32_C(0x80)) buffer[(*position)++] = (char)value;
    else if (value < UINT32_C(0x800)) {
        buffer[(*position)++] = (char)(UINT32_C(0xc0) | (value >> 6));
        buffer[(*position)++] = (char)(UINT32_C(0x80) | (value & UINT32_C(0x3f)));
    } else if (value < UINT32_C(0x10000)) {
        buffer[(*position)++] = (char)(UINT32_C(0xe0) | (value >> 12));
        buffer[(*position)++] = (char)(UINT32_C(0x80) | ((value >> 6) & UINT32_C(0x3f)));
        buffer[(*position)++] = (char)(UINT32_C(0x80) | (value & UINT32_C(0x3f)));
    } else {
        buffer[(*position)++] = (char)(UINT32_C(0xf0) | (value >> 18));
        buffer[(*position)++] = (char)(UINT32_C(0x80) | ((value >> 12) & UINT32_C(0x3f)));
        buffer[(*position)++] = (char)(UINT32_C(0x80) | ((value >> 6) & UINT32_C(0x3f)));
        buffer[(*position)++] = (char)(UINT32_C(0x80) | (value & UINT32_C(0x3f)));
    }
}

int romx_json_copy_string(const romx_json_document_t *document,
    int token_index, char *buffer, size_t capacity, size_t *required)
{
    const romx_json_token_t *token;
    size_t input;
    size_t output = 0U;
    if (token_index < 0 || (size_t)token_index >= document->token_count) return 0;
    token = &document->tokens[token_index];
    if (token->type != ROMX_JSON_STRING) return 0;
    input = token->start + 1U;
    while (input + 1U < token->end) {
        uint32_t value;
        const size_t before = input;
        if (!decode_next(document->bytes, token->end - 1U, &input, &value)) return 0;
        if (document->bytes[before] >= UINT8_C(0x80)) {
            size_t count = 1U;
            while (input < token->end - 1U &&
                (document->bytes[input] & UINT8_C(0xc0)) == UINT8_C(0x80)) { ++input; ++count; }
            if (buffer != NULL && output + count < capacity)
                memcpy(buffer + output, document->bytes + before, count);
            output += count;
        } else {
            const size_t count = utf8_size(value);
            if (buffer != NULL && output + count < capacity) write_utf8(buffer, &output, value);
            else output += count;
        }
    }
    if (required != NULL) *required = output + 1U;
    if (buffer == NULL || capacity < output + 1U) return 0;
    buffer[output] = '\0';
    return 1;
}

size_t romx_json_string_characters(const romx_json_document_t *document, int token_index)
{
    const romx_json_token_t *token = &document->tokens[token_index];
    size_t input = token->start + 1U;
    size_t count = 0U;
    while (input + 1U < token->end) {
        uint32_t value;
        const size_t before = input;
        if (!decode_next(document->bytes, token->end - 1U, &input, &value)) return SIZE_MAX;
        if (document->bytes[before] >= UINT8_C(0x80)) {
            while (input < token->end - 1U &&
                (document->bytes[input] & UINT8_C(0xc0)) == UINT8_C(0x80)) ++input;
        }
        ++count;
    }
    return count;
}

int romx_json_string_equals(const romx_json_document_t *document,
    int token_index, const char *value)
{
    size_t required = 0U;
    size_t value_size;
    char *decoded;
    int equal;

    if (value == NULL) return 0;
    (void)romx_json_copy_string(document, token_index, NULL, 0U, &required);
    value_size = strlen(value) + 1U;
    if (required != value_size) return 0;
    decoded = (char *)malloc(required);
    if (decoded == NULL) return 0;
    if (!romx_json_copy_string(document, token_index, decoded, required, &required)) {
        free(decoded); return 0;
    }
    equal = memcmp(decoded, value, required) == 0;
    free(decoded);
    return equal;
}

int romx_json_strings_equal(const romx_json_document_t *document,
    int first_token, int second_token)
{
    size_t first_size = 0U;
    size_t second_size = 0U;
    char *first;
    char *second;
    int equal;

    (void)romx_json_copy_string(document, first_token, NULL, 0U,
        &first_size);
    (void)romx_json_copy_string(document, second_token, NULL, 0U,
        &second_size);
    if (first_size == 0U || first_size != second_size) return 0;
    first = (char *)malloc(first_size);
    if (first == NULL) return 1; /* Fail closed during duplicate-key checks. */
    second = (char *)malloc(second_size);
    if (second == NULL) {
        free(first);
        return 1;
    }
    if (!romx_json_copy_string(document, first_token, first, first_size,
            &first_size) ||
        !romx_json_copy_string(document, second_token, second, second_size,
            &second_size)) {
        free(first);
        free(second);
        return 1;
    }
    equal = memcmp(first, second, first_size) == 0;
    free(first);
    free(second);
    return equal;
}

int romx_json_next_direct_child(const romx_json_document_t *document,
    int parent, int after)
{
    size_t index = after < 0 ? 0U : (size_t)after + 1U;
    for (; index < document->token_count; ++index)
        if (document->tokens[index].parent == parent) return (int)index;
    return -1;
}

int romx_json_object_value(const romx_json_document_t *document,
    int object_index, const char *key)
{
    int item = -1;
    if (object_index < 0 || (size_t)object_index >= document->token_count ||
        document->tokens[object_index].type != ROMX_JSON_OBJECT) return -1;
    for (;;) {
        int value;
        item = romx_json_next_direct_child(document, object_index, item);
        if (item < 0) return -1;
        value = romx_json_next_direct_child(document, object_index, item);
        if (value < 0) return -1;
        if (romx_json_string_equals(document, item, key)) return value;
        item = value;
    }
}

int romx_json_integer(const romx_json_document_t *document,
    int token_index, int64_t *value)
{
    const romx_json_token_t *token;
    size_t position;
    int negative = 0;
    uint64_t result = UINT64_C(0);
    if (token_index < 0 || (size_t)token_index >= document->token_count) return 0;
    token = &document->tokens[token_index];
    if (token->type != ROMX_JSON_NUMBER) return 0;
    position = token->start;
    if (document->bytes[position] == (uint8_t)'-') { negative = 1; ++position; }
    for (; position < token->end; ++position) {
        const uint8_t byte = document->bytes[position];
        if (byte < (uint8_t)'0' || byte > (uint8_t)'9') return 0;
        if (result > (UINT64_MAX - (uint64_t)(byte - (uint8_t)'0')) / UINT64_C(10)) return 0;
        result = result * UINT64_C(10) + (uint64_t)(byte - (uint8_t)'0');
    }
    if ((!negative && result > (uint64_t)INT64_MAX) ||
        (negative && result > (uint64_t)INT64_MAX + UINT64_C(1))) return 0;
    if (negative && result == (uint64_t)INT64_MAX + UINT64_C(1)) *value = INT64_MIN;
    else *value = negative ? -(int64_t)result : (int64_t)result;
    return 1;
}
