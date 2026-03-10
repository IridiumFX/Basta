#ifndef BASTA_H
#define BASTA_H

#include <stddef.h>
#include <stdint.h>

/* DLL export/import (BASTA_STATIC disables for static builds) */
#ifdef BASTA_STATIC
  #define BASTA_API
#elif defined(_WIN32)
  #ifdef BASTA_BUILDING
    #define BASTA_API __declspec(dllexport)
  #else
    #define BASTA_API __declspec(dllimport)
  #endif
#else
  #define BASTA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Value types.
   BASTA_BLOB is the only addition over Pasta's type set.  All other
   types have identical semantics to their Pasta counterparts. */
typedef enum {
    BASTA_NULL,
    BASTA_BOOL,
    BASTA_NUMBER,
    BASTA_STRING,
    BASTA_ARRAY,
    BASTA_MAP,
    BASTA_LABEL,
    BASTA_BLOB      /* raw binary blob */
} BastaType;

/* Opaque value handle */
typedef struct BastaValue BastaValue;

/* Error codes */
typedef enum {
    BASTA_OK = 0,
    BASTA_ERR_ALLOC,
    BASTA_ERR_SYNTAX,
    BASTA_ERR_UNEXPECTED_TOKEN,
    BASTA_ERR_UNEXPECTED_EOF,
    BASTA_ERR_BLOB_TRUNCATED    /* sentinel found but size/data bytes missing */
} BastaError;

/* Error info */
typedef struct {
    BastaError code;
    int        line;
    int        col;
    int        sections;    /* 1 if the document used @name sections */
    char       message[256];
} BastaResult;

/* ---- Parsing ---- */

/* Parse a Basta document from a buffer.  The buffer may contain embedded
   binary blobs; len must be the exact byte count of the input.
   Returns a BastaValue* on success or NULL on error.
   The optional BastaResult receives error details. */
BASTA_API BastaValue *basta_parse(const char *input, size_t len, BastaResult *result);

/* Convenience: parse a null-terminated string.  Suitable for Basta
   documents that contain no blobs (i.e. pure-text Pasta-compatible
   documents). */
BASTA_API BastaValue *basta_parse_cstr(const char *input, BastaResult *result);

BASTA_API void        basta_free(BastaValue *value);

/* ---- Querying ---- */

BASTA_API BastaType    basta_type(const BastaValue *v);
BASTA_API int          basta_is_null(const BastaValue *v);

/* Scalars */
BASTA_API int          basta_get_bool(const BastaValue *v);
BASTA_API double       basta_get_number(const BastaValue *v);
BASTA_API const char  *basta_get_string(const BastaValue *v);
BASTA_API size_t       basta_get_string_len(const BastaValue *v);
BASTA_API const char  *basta_get_label(const BastaValue *v);
BASTA_API size_t       basta_get_label_len(const BastaValue *v);

/* Blob accessor.  Returns a pointer into the value tree (valid until
   basta_free).  Sets *out_len to the blob byte count.
   Returns NULL and sets *out_len to 0 if v is not a BASTA_BLOB. */
BASTA_API const uint8_t *basta_get_blob(const BastaValue *v, size_t *out_len);

/* Containers */
BASTA_API size_t             basta_count(const BastaValue *v);
BASTA_API const BastaValue  *basta_array_get(const BastaValue *v, size_t index);
BASTA_API const BastaValue  *basta_map_get(const BastaValue *v, const char *key);
BASTA_API const char        *basta_map_key(const BastaValue *v, size_t index);
BASTA_API const BastaValue  *basta_map_value(const BastaValue *v, size_t index);

/* ---- Building ---- */

BASTA_API BastaValue *basta_new_null(void);
BASTA_API BastaValue *basta_new_bool(int b);
BASTA_API BastaValue *basta_new_number(double n);
BASTA_API BastaValue *basta_new_string(const char *s);
BASTA_API BastaValue *basta_new_string_len(const char *s, size_t len);
BASTA_API BastaValue *basta_new_label(const char *s);
BASTA_API BastaValue *basta_new_label_len(const char *s, size_t len);
BASTA_API BastaValue *basta_new_array(void);
BASTA_API BastaValue *basta_new_map(void);

/* Blob constructor.  Copies len bytes from data.
   Caller retains ownership of the data pointer. */
BASTA_API BastaValue *basta_new_blob(const uint8_t *data, size_t len);

/* Mutators — returns 0 on success, -1 on error. Container takes ownership. */
BASTA_API int basta_push(BastaValue *array, BastaValue *item);
BASTA_API int basta_set(BastaValue *map, const char *key, BastaValue *value);
BASTA_API int basta_set_len(BastaValue *map, const char *key, size_t key_len, BastaValue *value);

/* ---- Writing ---- */

/* Flags for basta_write */
#define BASTA_PRETTY   0   /* indented, multiline (default) */
#define BASTA_COMPACT  1   /* single-line, minimal whitespace */
#define BASTA_SECTIONS 2   /* emit root map as @section containers */
#define BASTA_SORTED   4   /* sort map keys lexicographically */

/* Serialize a value tree to a malloc'd buffer.  The buffer is always
   null-terminated, but may contain embedded 0x00 bytes if the tree
   holds blobs.  If out_len is non-NULL it receives the byte count of
   the document (excluding the null terminator).  Caller must free().
   Returns NULL on allocation failure. */
BASTA_API char *basta_write(const BastaValue *v, int flags, size_t *out_len);

/* Write to a FILE*.  Writes raw bytes including any binary blob data.
   Returns 0 on success, -1 on error. */
BASTA_API int   basta_write_fp(const BastaValue *v, int flags, void *fp);

#ifdef __cplusplus
}
#endif

#endif /* BASTA_H */
