#include "basta_internal.h"

static BastaValue *alloc_value(BastaType type) {
    BastaValue *v = (BastaValue *)calloc(1, sizeof(BastaValue));
    if (v) v->type = type;
    return v;
}

BastaValue *basta_value_null(void) {
    return alloc_value(BASTA_NULL);
}

BastaValue *basta_value_bool(int b) {
    BastaValue *v = alloc_value(BASTA_BOOL);
    if (v) v->as.boolean = b;
    return v;
}

BastaValue *basta_value_number(double n) {
    BastaValue *v = alloc_value(BASTA_NUMBER);
    if (v) v->as.number = n;
    return v;
}

BastaValue *basta_value_string(const char *s, size_t len) {
    BastaValue *v = alloc_value(BASTA_STRING);
    if (!v) return NULL;
    v->as.string.data = (char *)malloc(len + 1);
    if (!v->as.string.data) { free(v); return NULL; }
    memcpy(v->as.string.data, s, len);
    v->as.string.data[len] = '\0';
    v->as.string.len = len;
    return v;
}

BastaValue *basta_value_label(const char *s, size_t len) {
    BastaValue *v = alloc_value(BASTA_LABEL);
    if (!v) return NULL;
    v->as.string.data = (char *)malloc(len + 1);
    if (!v->as.string.data) { free(v); return NULL; }
    memcpy(v->as.string.data, s, len);
    v->as.string.data[len] = '\0';
    v->as.string.len = len;
    return v;
}

BastaValue *basta_value_blob(const uint8_t *data, size_t len) {
    BastaValue *v = alloc_value(BASTA_BLOB);
    if (!v) return NULL;
    v->as.blob.data = (uint8_t *)malloc(len ? len : 1);
    if (!v->as.blob.data) { free(v); return NULL; }
    if (data && len) memcpy(v->as.blob.data, data, len);
    v->as.blob.len = len;
    return v;
}

BastaValue *basta_value_array(void) {
    BastaValue *v = alloc_value(BASTA_ARRAY);
    if (!v) return NULL;
    v->as.array.cap   = 8;
    v->as.array.items = (BastaValue **)malloc(sizeof(BastaValue *) * v->as.array.cap);
    if (!v->as.array.items) { free(v); return NULL; }
    return v;
}

BastaValue *basta_value_map(void) {
    BastaValue *v = alloc_value(BASTA_MAP);
    if (!v) return NULL;
    v->as.map.cap   = 8;
    v->as.map.items = (BastaMember *)malloc(sizeof(BastaMember) * v->as.map.cap);
    if (!v->as.map.items) { free(v); return NULL; }
    return v;
}

int basta_array_push(BastaValue *arr, BastaValue *item) {
    if (arr->as.array.count >= arr->as.array.cap) {
        size_t new_cap = arr->as.array.cap * 2;
        BastaValue **tmp = (BastaValue **)realloc(arr->as.array.items, sizeof(BastaValue *) * new_cap);
        if (!tmp) return -1;
        arr->as.array.items = tmp;
        arr->as.array.cap   = new_cap;
    }
    arr->as.array.items[arr->as.array.count++] = item;
    return 0;
}

int basta_map_put(BastaValue *map, const char *key, size_t key_len, BastaValue *value) {
    if (map->as.map.count >= map->as.map.cap) {
        size_t new_cap = map->as.map.cap * 2;
        BastaMember *tmp = (BastaMember *)realloc(map->as.map.items, sizeof(BastaMember) * new_cap);
        if (!tmp) return -1;
        map->as.map.items = tmp;
        map->as.map.cap   = new_cap;
    }
    char *k = (char *)malloc(key_len + 1);
    if (!k) return -1;
    memcpy(k, key, key_len);
    k[key_len] = '\0';
    map->as.map.items[map->as.map.count].key   = k;
    map->as.map.items[map->as.map.count].value = value;
    map->as.map.count++;
    return 0;
}

/* ---- Public Builder API ---- */

BASTA_API BastaValue *basta_new_null(void) { return basta_value_null(); }
BASTA_API BastaValue *basta_new_bool(int b) { return basta_value_bool(b); }
BASTA_API BastaValue *basta_new_number(double n) { return basta_value_number(n); }
BASTA_API BastaValue *basta_new_string(const char *s) {
    return basta_value_string(s, s ? strlen(s) : 0);
}
BASTA_API BastaValue *basta_new_string_len(const char *s, size_t len) {
    return basta_value_string(s, len);
}
BASTA_API BastaValue *basta_new_label(const char *s) {
    return basta_value_label(s, s ? strlen(s) : 0);
}
BASTA_API BastaValue *basta_new_label_len(const char *s, size_t len) {
    return basta_value_label(s, len);
}
BASTA_API BastaValue *basta_new_array(void) { return basta_value_array(); }
BASTA_API BastaValue *basta_new_map(void)   { return basta_value_map();   }
BASTA_API BastaValue *basta_new_blob(const uint8_t *data, size_t len) {
    return basta_value_blob(data, len);
}

BASTA_API int basta_push(BastaValue *array, BastaValue *item) {
    if (!array || array->type != BASTA_ARRAY || !item) return -1;
    return basta_array_push(array, item);
}

BASTA_API int basta_set(BastaValue *map, const char *key, BastaValue *value) {
    if (!map || map->type != BASTA_MAP || !key || !value) return -1;
    return basta_map_put(map, key, strlen(key), value);
}

BASTA_API int basta_set_len(BastaValue *map, const char *key, size_t key_len, BastaValue *value) {
    if (!map || map->type != BASTA_MAP || !key || !value) return -1;
    return basta_map_put(map, key, key_len, value);
}

/* ---- Memory ---- */

BASTA_API void basta_free(BastaValue *v) {
    if (!v) return;
    switch (v->type) {
        case BASTA_STRING:
        case BASTA_LABEL:
            free(v->as.string.data);
            break;
        case BASTA_BLOB:
            free(v->as.blob.data);
            break;
        case BASTA_ARRAY:
            for (size_t i = 0; i < v->as.array.count; i++)
                basta_free(v->as.array.items[i]);
            free(v->as.array.items);
            break;
        case BASTA_MAP:
            for (size_t i = 0; i < v->as.map.count; i++) {
                free(v->as.map.items[i].key);
                basta_free(v->as.map.items[i].value);
            }
            free(v->as.map.items);
            break;
        default:
            break;
    }
    free(v);
}

/* ---- Public Query API ---- */

BASTA_API BastaType basta_type(const BastaValue *v) {
    return v ? v->type : BASTA_NULL;
}

BASTA_API int basta_is_null(const BastaValue *v) {
    return !v || v->type == BASTA_NULL;
}

BASTA_API int basta_get_bool(const BastaValue *v) {
    return (v && v->type == BASTA_BOOL) ? v->as.boolean : 0;
}

BASTA_API double basta_get_number(const BastaValue *v) {
    return (v && v->type == BASTA_NUMBER) ? v->as.number : 0.0;
}

BASTA_API const char *basta_get_string(const BastaValue *v) {
    return (v && v->type == BASTA_STRING) ? v->as.string.data : NULL;
}

BASTA_API size_t basta_get_string_len(const BastaValue *v) {
    return (v && v->type == BASTA_STRING) ? v->as.string.len : 0;
}

BASTA_API const char *basta_get_label(const BastaValue *v) {
    return (v && v->type == BASTA_LABEL) ? v->as.string.data : NULL;
}

BASTA_API size_t basta_get_label_len(const BastaValue *v) {
    return (v && v->type == BASTA_LABEL) ? v->as.string.len : 0;
}

BASTA_API const uint8_t *basta_get_blob(const BastaValue *v, size_t *out_len) {
    if (v && v->type == BASTA_BLOB) {
        if (out_len) *out_len = v->as.blob.len;
        return v->as.blob.data;
    }
    if (out_len) *out_len = 0;
    return NULL;
}

BASTA_API size_t basta_count(const BastaValue *v) {
    if (!v) return 0;
    if (v->type == BASTA_ARRAY) return v->as.array.count;
    if (v->type == BASTA_MAP)   return v->as.map.count;
    return 0;
}

BASTA_API const BastaValue *basta_array_get(const BastaValue *v, size_t index) {
    if (!v || v->type != BASTA_ARRAY || index >= v->as.array.count) return NULL;
    return v->as.array.items[index];
}

BASTA_API const BastaValue *basta_map_get(const BastaValue *v, const char *key) {
    if (!v || v->type != BASTA_MAP || !key) return NULL;
    for (size_t i = 0; i < v->as.map.count; i++) {
        if (strcmp(v->as.map.items[i].key, key) == 0)
            return v->as.map.items[i].value;
    }
    return NULL;
}

BASTA_API const char *basta_map_key(const BastaValue *v, size_t index) {
    if (!v || v->type != BASTA_MAP || index >= v->as.map.count) return NULL;
    return v->as.map.items[index].key;
}

BASTA_API const BastaValue *basta_map_value(const BastaValue *v, size_t index) {
    if (!v || v->type != BASTA_MAP || index >= v->as.map.count) return NULL;
    return v->as.map.items[index].value;
}
