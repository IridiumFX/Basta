#include "basta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/*  Test harness                                                       */
/* ------------------------------------------------------------------ */

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond) do { \
    if (cond) { g_passed++; } \
    else { fprintf(stderr, "FAIL: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); g_failed++; } \
} while (0)

#define SECTION(name) printf("\n--- %s ---\n", name)

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Build a Basta document buffer that contains an embedded blob.
   Layout: text_before + 0x00 + uint64be(blob_len) + blob_data + text_after
   Caller must free the returned buffer. *out_len receives byte count. */
static char *make_basta_doc(
    const char *before, size_t before_len,
    const uint8_t *blob, uint64_t blob_len,
    const char *after, size_t after_len,
    size_t *out_len)
{
    size_t total = before_len + 1 + 8 + (size_t)blob_len + after_len;
    char *buf = (char *)malloc(total + 1);
    if (!buf) return NULL;

    size_t pos = 0;
    memcpy(buf + pos, before, before_len); pos += before_len;
    buf[pos++] = 0x00;
    for (int i = 7; i >= 0; i--) buf[pos++] = (char)((blob_len >> (i * 8)) & 0xFF);
    memcpy(buf + pos, blob, (size_t)blob_len); pos += (size_t)blob_len;
    memcpy(buf + pos, after, after_len); pos += after_len;
    buf[pos] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

/* Portable memmem for the test suite (memmem is not in C11 stdlib) */
static void *test_memmem(const void *hay, size_t hay_len,
                          const void *needle, size_t needle_len) {
    if (needle_len == 0) return (void *)hay;
    const char *h = (const char *)hay;
    const char *n = (const char *)needle;
    for (size_t i = 0; i + needle_len <= hay_len; i++)
        if (memcmp(h + i, n, needle_len) == 0) return (void *)(h + i);
    return NULL;
}



static void test_text_only(void) {
    SECTION("text-only (Pasta compat)");

    BastaResult r;
    BastaValue *v = basta_parse_cstr(
        "{host: \"localhost\", port: 5432, ssl: true, timeout: null}",
        &r);
    CHECK(v != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_type(v) == BASTA_MAP);
    CHECK(basta_count(v) == 4);

    const BastaValue *host = basta_map_get(v, "host");
    CHECK(host != NULL);
    CHECK(basta_type(host) == BASTA_STRING);
    CHECK(strcmp(basta_get_string(host), "localhost") == 0);

    const BastaValue *port = basta_map_get(v, "port");
    CHECK(basta_type(port) == BASTA_NUMBER);
    CHECK(basta_get_number(port) == 5432.0);

    const BastaValue *ssl = basta_map_get(v, "ssl");
    CHECK(basta_type(ssl) == BASTA_BOOL);
    CHECK(basta_get_bool(ssl) == 1);

    CHECK(basta_is_null(basta_map_get(v, "timeout")));

    basta_free(v);
}

/* ------------------------------------------------------------------ */
/*  2. Blob construction and query                                     */
/* ------------------------------------------------------------------ */

static void test_blob_value(void) {
    SECTION("blob value");

    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x01};
    BastaValue *b = basta_new_blob(data, sizeof(data));
    CHECK(b != NULL);
    CHECK(basta_type(b) == BASTA_BLOB);

    size_t len = 0;
    const uint8_t *got = basta_get_blob(b, &len);
    CHECK(got != NULL);
    CHECK(len == sizeof(data));
    CHECK(memcmp(got, data, len) == 0);

    /* zero-length blob */
    BastaValue *empty = basta_new_blob(NULL, 0);
    CHECK(empty != NULL);
    CHECK(basta_type(empty) == BASTA_BLOB);
    size_t elen = 99;
    basta_get_blob(empty, &elen);
    CHECK(elen == 0);

    /* type safety: blob accessor returns NULL for non-blob */
    BastaValue *num = basta_new_number(42.0);
    size_t dummy;
    CHECK(basta_get_blob(num, &dummy) == NULL);
    CHECK(dummy == 0);

    basta_free(b);
    basta_free(empty);
    basta_free(num);
}

/* ------------------------------------------------------------------ */
/*  3. Blob as map value                                               */
/* ------------------------------------------------------------------ */

static void test_blob_in_map(void) {
    SECTION("blob in map");

    static const uint8_t pixels[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    BastaValue *doc = basta_new_map();
    basta_set(doc, "format", basta_new_string("png"));
    basta_set(doc, "width",  basta_new_number(2));
    basta_set(doc, "height", basta_new_number(1));
    basta_set(doc, "size",   basta_new_number(sizeof(pixels)));
    basta_set(doc, "data",   basta_new_blob(pixels, sizeof(pixels)));

    CHECK(basta_count(doc) == 5);

    const BastaValue *data_val = basta_map_get(doc, "data");
    CHECK(data_val != NULL);
    CHECK(basta_type(data_val) == BASTA_BLOB);

    size_t len;
    const uint8_t *got = basta_get_blob(data_val, &len);
    CHECK(len == sizeof(pixels));
    CHECK(memcmp(got, pixels, len) == 0);

    /* cross-validate size field */
    double declared = basta_get_number(basta_map_get(doc, "size"));
    CHECK((size_t)declared == len);

    basta_free(doc);
}

/* ------------------------------------------------------------------ */
/*  4. Blob in array                                                   */
/* ------------------------------------------------------------------ */

static void test_blob_in_array(void) {
    SECTION("blob in array");

    BastaValue *arr = basta_new_array();
    uint8_t a[] = {0x01, 0x02};
    uint8_t b[] = {0xAA, 0xBB, 0xCC};
    basta_push(arr, basta_new_string("before"));
    basta_push(arr, basta_new_blob(a, sizeof(a)));
    basta_push(arr, basta_new_blob(b, sizeof(b)));
    basta_push(arr, basta_new_string("after"));

    CHECK(basta_count(arr) == 4);
    CHECK(basta_type(basta_array_get(arr, 0)) == BASTA_STRING);
    CHECK(basta_type(basta_array_get(arr, 1)) == BASTA_BLOB);
    CHECK(basta_type(basta_array_get(arr, 2)) == BASTA_BLOB);
    CHECK(basta_type(basta_array_get(arr, 3)) == BASTA_STRING);

    size_t len;
    basta_get_blob(basta_array_get(arr, 1), &len);
    CHECK(len == 2);
    basta_get_blob(basta_array_get(arr, 2), &len);
    CHECK(len == 3);

    basta_free(arr);
}

/* ------------------------------------------------------------------ */
/*  5. Write / parse roundtrip for a document with a blob             */
/* ------------------------------------------------------------------ */

static void test_roundtrip(void) {
    SECTION("write/parse roundtrip");

    static const uint8_t payload[] = {
        0x00, 0x01, 0x02, 0x03, 0x7F, 0x80, 0xFE, 0xFF
    };

    /* build */
    BastaValue *orig = basta_new_map();
    basta_set(orig, "type",    basta_new_string("raw"));
    basta_set(orig, "size",    basta_new_number(sizeof(payload)));
    basta_set(orig, "data",    basta_new_blob(payload, sizeof(payload)));
    basta_set(orig, "comment", basta_new_string("includes zero bytes"));

    /* write */
    size_t doc_len;
    char *doc = basta_write(orig, BASTA_PRETTY, &doc_len);
    CHECK(doc != NULL);
    CHECK(doc_len > 0);

    /* doc contains the sentinel + 8-byte size + payload embedded in text */

    /* parse back */
    BastaResult r;
    BastaValue *copy = basta_parse(doc, doc_len, &r);
    CHECK(copy != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_type(copy) == BASTA_MAP);

    /* verify text fields */
    CHECK(strcmp(basta_get_string(basta_map_get(copy, "type")), "raw") == 0);
    CHECK(basta_get_number(basta_map_get(copy, "size")) == (double)sizeof(payload));
    CHECK(strcmp(basta_get_string(basta_map_get(copy, "comment")), "includes zero bytes") == 0);

    /* verify blob */
    size_t got_len;
    const uint8_t *got = basta_get_blob(basta_map_get(copy, "data"), &got_len);
    CHECK(got != NULL);
    CHECK(got_len == sizeof(payload));
    CHECK(memcmp(got, payload, got_len) == 0);

    free(doc);
    basta_free(orig);
    basta_free(copy);
}

/* ------------------------------------------------------------------ */
/*  6. Parse: blob embedded in a manually constructed document        */
/* ------------------------------------------------------------------ */

static void test_parse_blob(void) {
    SECTION("parse blob from wire format");

    static const uint8_t blob_data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; /* "Hello" */

    const char *before = "{greeting: ";
    const char *after  = "}";
    size_t doc_len;
    char *doc = make_basta_doc(
        before, strlen(before),
        blob_data, sizeof(blob_data),
        after, strlen(after),
        &doc_len);
    CHECK(doc != NULL);

    BastaResult r;
    BastaValue *v = basta_parse(doc, doc_len, &r);
    CHECK(v != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_type(v) == BASTA_MAP);

    size_t len;
    const uint8_t *got = basta_get_blob(basta_map_get(v, "greeting"), &len);
    CHECK(got != NULL);
    CHECK(len == sizeof(blob_data));
    CHECK(memcmp(got, blob_data, len) == 0);

    free(doc);
    basta_free(v);
}

/* ------------------------------------------------------------------ */
/*  7. Parse: multiple blobs in one document                          */
/* ------------------------------------------------------------------ */

static void test_parse_multiple_blobs(void) {
    SECTION("multiple blobs in one document");

    uint8_t b1[] = {0x01, 0x02, 0x03};
    uint8_t b2[] = {0xAA, 0xBB};
    uint8_t b3[] = {0xFF};

    /* Build: {a: <blob1>, b: <blob2>, c: <blob3>} */
    BastaValue *doc = basta_new_map();
    basta_set(doc, "a", basta_new_blob(b1, sizeof(b1)));
    basta_set(doc, "b", basta_new_blob(b2, sizeof(b2)));
    basta_set(doc, "c", basta_new_blob(b3, sizeof(b3)));

    size_t doc_len;
    char *buf = basta_write(doc, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    size_t len;
    const uint8_t *ga = basta_get_blob(basta_map_get(back, "a"), &len);
    CHECK(len == 3 && ga && memcmp(ga, b1, 3) == 0);

    const uint8_t *gb = basta_get_blob(basta_map_get(back, "b"), &len);
    CHECK(len == 2 && gb && memcmp(gb, b2, 2) == 0);

    const uint8_t *gc = basta_get_blob(basta_map_get(back, "c"), &len);
    CHECK(len == 1 && gc && gc[0] == 0xFF);

    free(buf);
    basta_free(doc);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  8. Zero-byte blob                                                  */
/* ------------------------------------------------------------------ */

static void test_empty_blob(void) {
    SECTION("zero-length blob");

    BastaValue *v = basta_new_map();
    basta_set(v, "empty", basta_new_blob(NULL, 0));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    size_t len = 99;
    const uint8_t *got = basta_get_blob(basta_map_get(back, "empty"), &len);
    CHECK(got != NULL);   /* pointer is valid (malloc'd at least 1 byte) */
    CHECK(len == 0);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  9. Blob with 0x00 bytes inside payload                             */
/* ------------------------------------------------------------------ */

static void test_blob_containing_zeros(void) {
    SECTION("blob payload containing 0x00 bytes");

    uint8_t data[] = {0x00, 0x01, 0x00, 0xFF, 0x00};
    BastaValue *v = basta_new_map();
    basta_set(v, "d", basta_new_blob(data, sizeof(data)));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    size_t len;
    const uint8_t *got = basta_get_blob(basta_map_get(back, "d"), &len);
    CHECK(len == sizeof(data));
    CHECK(memcmp(got, data, len) == 0);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  10. Named sections with blobs                                      */
/* ------------------------------------------------------------------ */

static void test_sections_with_blobs(void) {
    SECTION("sections with blobs");

    uint8_t icon[]   = {0x89, 0x50, 0x4E, 0x47};  /* PNG magic */
    uint8_t shader[] = {0x03, 0x02, 0x23, 0x07};  /* SPIR-V magic */

    BastaValue *root = basta_new_map();

    BastaValue *meta = basta_new_map();
    basta_set(meta, "generator", basta_new_string("basta-pack"));
    basta_set(root, "meta", meta);

    BastaValue *assets = basta_new_map();
    basta_set(assets, "icon",   basta_new_blob(icon,   sizeof(icon)));
    basta_set(assets, "shader", basta_new_blob(shader, sizeof(shader)));
    basta_set(root, "assets", assets);

    size_t doc_len;
    char *buf = basta_write(root, BASTA_SECTIONS | BASTA_PRETTY, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    const BastaValue *back_assets = basta_map_get(back, "assets");
    CHECK(back_assets != NULL);

    size_t len;
    const uint8_t *gi = basta_get_blob(basta_map_get(back_assets, "icon"), &len);
    CHECK(len == sizeof(icon) && gi && memcmp(gi, icon, len) == 0);

    const uint8_t *gs = basta_get_blob(basta_map_get(back_assets, "shader"), &len);
    CHECK(len == sizeof(shader) && gs && memcmp(gs, shader, len) == 0);

    free(buf);
    basta_free(root);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  11. Error: truncated blob (missing size bytes)                     */
/* ------------------------------------------------------------------ */

static void test_error_truncated_size(void) {
    SECTION("error: truncated size field");

    /* Only 4 of the 8 size bytes present */
    char buf[] = {'{', 'x', ':', ' ', 0x00, 0x00, 0x00, 0x00, 0x05, '}'};
    BastaResult r;
    BastaValue *v = basta_parse(buf, sizeof(buf), &r);
    CHECK(v == NULL);
    CHECK(r.code == BASTA_ERR_BLOB_TRUNCATED);
}

/* ------------------------------------------------------------------ */
/*  12. Error: truncated blob data                                     */
/* ------------------------------------------------------------------ */

static void test_error_truncated_data(void) {
    SECTION("error: truncated blob data");

    /* Size says 10 bytes but only 3 follow */
    const char *before = "{x: ";
    const char *after  = "}";
    uint8_t short_data[] = {0xAA, 0xBB, 0xCC};

    /* Build a doc claiming blob_len=10 but only supply 3 data bytes */
    size_t blen = strlen(before) + 1 + 8 + 3 + strlen(after);
    char *doc = (char *)malloc(blen + 1);
    size_t pos = 0;
    memcpy(doc + pos, before, strlen(before)); pos += strlen(before);
    doc[pos++] = 0x00;
    /* 8-byte BE size = 10 */
    uint64_t claimed = 10;
    for (int i = 7; i >= 0; i--) doc[pos++] = (char)((claimed >> (i*8)) & 0xFF);
    memcpy(doc + pos, short_data, 3); pos += 3;
    memcpy(doc + pos, after, strlen(after)); pos += strlen(after);
    doc[pos] = '\0';

    BastaResult r;
    BastaValue *v = basta_parse(doc, blen, &r);
    CHECK(v == NULL);
    CHECK(r.code == BASTA_ERR_BLOB_TRUNCATED);

    free(doc);
}

/* ------------------------------------------------------------------ */
/*  13. SORTED flag works across text and blob values                  */
/* ------------------------------------------------------------------ */

static void test_sorted_flag(void) {
    SECTION("SORTED flag");

    uint8_t bd[] = {0x42};
    BastaValue *v = basta_new_map();
    basta_set(v, "zzz",  basta_new_string("last"));
    basta_set(v, "aaa",  basta_new_string("first"));
    basta_set(v, "mmm",  basta_new_blob(bd, sizeof(bd)));

    size_t len;
    char *buf = basta_write(v, BASTA_COMPACT | BASTA_SORTED, &len);
    CHECK(buf != NULL);

    /* aaa should appear before mmm before zzz in the output text.
       Use memmem because the buffer contains embedded 0x00 blob bytes. */
    char *paaa = (char *)test_memmem(buf, len, "aaa", 3);
    char *pmmm = (char *)test_memmem(buf, len, "mmm", 3);
    char *pzzz = (char *)test_memmem(buf, len, "zzz", 3);
    CHECK(paaa && pmmm && pzzz);
    CHECK(paaa < pmmm && pmmm < pzzz);

    free(buf);
    basta_free(v);
}

/* ------------------------------------------------------------------ */
/*  14. out_len correctness                                            */
/* ------------------------------------------------------------------ */

static void test_out_len(void) {
    SECTION("basta_write out_len");

    uint8_t data[] = {0xDE, 0xAD};
    BastaValue *v = basta_new_map();
    basta_set(v, "d", basta_new_blob(data, sizeof(data)));

    size_t out_len = 0;
    char *buf = basta_write(v, BASTA_COMPACT, &out_len);
    CHECK(buf != NULL);
    CHECK(out_len > 0);

    /* buf[out_len] must be '\0' (null terminator past the end) */
    CHECK(buf[out_len] == '\0');

    /* The buffer must contain the sentinel 0x00 somewhere */
    int found_sentinel = 0;
    for (size_t i = 0; i < out_len; i++) {
        if ((unsigned char)buf[i] == 0x00) { found_sentinel = 1; break; }
    }
    CHECK(found_sentinel);

    free(buf);
    basta_free(v);
}

/* ------------------------------------------------------------------ */
/*  15. Map of blobs — every value is a blob                          */
/* ------------------------------------------------------------------ */

static void test_map_of_blobs(void) {
    SECTION("map of blobs");

#define N_BLOBS 32
    /* Generate N_BLOBS entries each with a distinct known payload */
    uint8_t payloads[N_BLOBS][64];
    char    keys[N_BLOBS][16];
    size_t  lens[N_BLOBS];

    for (int i = 0; i < N_BLOBS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "blob%02d", i);
        lens[i] = (size_t)(i + 1);        /* lengths 1..N_BLOBS */
        for (size_t j = 0; j < lens[i]; j++)
            payloads[i][j] = (uint8_t)((i * 7 + j * 13) & 0xFF);
    }

    BastaValue *map = basta_new_map();
    for (int i = 0; i < N_BLOBS; i++)
        basta_set(map, keys[i], basta_new_blob(payloads[i], lens[i]));

    CHECK(basta_count(map) == N_BLOBS);

    /* Write and parse back */
    size_t doc_len;
    char *buf = basta_write(map, BASTA_PRETTY, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_count(back) == N_BLOBS);

    /* Verify every entry */
    int all_ok = 1;
    for (int i = 0; i < N_BLOBS; i++) {
        size_t got_len;
        const uint8_t *got = basta_get_blob(basta_map_get(back, keys[i]), &got_len);
        if (!got || got_len != lens[i] || memcmp(got, payloads[i], lens[i]) != 0)
            all_ok = 0;
    }
    CHECK(all_ok);

    free(buf);
    basta_free(map);
    basta_free(back);
#undef N_BLOBS
}

/* ------------------------------------------------------------------ */
/*  16. Array of blobs — every element is a blob                      */
/* ------------------------------------------------------------------ */

static void test_array_of_blobs(void) {
    SECTION("array of blobs");

#define N_FRAMES 16
    uint8_t frames[N_FRAMES][256];
    for (int i = 0; i < N_FRAMES; i++)
        for (int j = 0; j < 256; j++)
            frames[i][j] = (uint8_t)((i + j) & 0xFF);

    BastaValue *arr = basta_new_array();
    for (int i = 0; i < N_FRAMES; i++)
        basta_push(arr, basta_new_blob(frames[i], 256));

    CHECK(basta_count(arr) == N_FRAMES);

    size_t doc_len;
    char *buf = basta_write(arr, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_count(back) == N_FRAMES);

    int all_ok = 1;
    for (int i = 0; i < N_FRAMES; i++) {
        size_t gl;
        const uint8_t *got = basta_get_blob(basta_array_get(back, (size_t)i), &gl);
        if (!got || gl != 256 || memcmp(got, frames[i], 256) != 0)
            all_ok = 0;
    }
    CHECK(all_ok);

    free(buf);
    basta_free(arr);
    basta_free(back);
#undef N_FRAMES
}

/* ------------------------------------------------------------------ */
/*  17. Large blob (1 MiB)                                             */
/* ------------------------------------------------------------------ */

static void test_large_blob(void) {
    SECTION("large blob (1 MiB)");

    size_t sz = 1024 * 1024;
    uint8_t *data = (uint8_t *)malloc(sz);
    CHECK(data != NULL);
    if (!data) return;

    /* fill with a deterministic pattern */
    for (size_t i = 0; i < sz; i++)
        data[i] = (uint8_t)(i & 0xFF);

    BastaValue *v = basta_new_map();
    basta_set(v, "size", basta_new_number((double)sz));
    basta_set(v, "data", basta_new_blob(data, sz));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);
    /* doc_len must be at least 1 + 8 + sz (sentinel + size_field + payload) */
    CHECK(doc_len >= 1 + 8 + sz);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    size_t got_len;
    const uint8_t *got = basta_get_blob(basta_map_get(back, "data"), &got_len);
    CHECK(got != NULL);
    CHECK(got_len == sz);

    /* spot-check a few bytes rather than memcmp of 1 MiB */
    int pattern_ok = 1;
    for (size_t i = 0; i < sz; i += 4096) {
        if (got[i] != (uint8_t)(i & 0xFF)) { pattern_ok = 0; break; }
    }
    CHECK(pattern_ok);

    free(data);
    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  18. Blob interleaved with all other value types                    */
/* ------------------------------------------------------------------ */

static void test_blob_interleaved_types(void) {
    SECTION("blob interleaved with all value types");

    uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};

    BastaValue *v = basta_new_map();
    basta_set(v, "a_null",   basta_new_null());
    basta_set(v, "b_bool",   basta_new_bool(1));
    basta_set(v, "c_number", basta_new_number(3.14));
    basta_set(v, "d_string", basta_new_string("hello"));
    basta_set(v, "e_blob",   basta_new_blob(payload, sizeof(payload)));
    basta_set(v, "f_label",  basta_new_label("ref"));
    basta_set(v, "g_array",  basta_new_array());
    basta_set(v, "h_map",    basta_new_map());
    basta_set(v, "i_blob2",  basta_new_blob(payload, sizeof(payload)));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_PRETTY, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_count(back) == 9);

    CHECK(basta_type(basta_map_get(back, "a_null"))   == BASTA_NULL);
    CHECK(basta_type(basta_map_get(back, "b_bool"))   == BASTA_BOOL);
    CHECK(basta_type(basta_map_get(back, "c_number")) == BASTA_NUMBER);
    CHECK(basta_type(basta_map_get(back, "d_string")) == BASTA_STRING);
    CHECK(basta_type(basta_map_get(back, "e_blob"))   == BASTA_BLOB);
    CHECK(basta_type(basta_map_get(back, "f_label"))  == BASTA_LABEL);
    CHECK(basta_type(basta_map_get(back, "g_array"))  == BASTA_ARRAY);
    CHECK(basta_type(basta_map_get(back, "h_map"))    == BASTA_MAP);
    CHECK(basta_type(basta_map_get(back, "i_blob2"))  == BASTA_BLOB);

    /* Both blobs have correct content */
    size_t la, lb;
    const uint8_t *ga = basta_get_blob(basta_map_get(back, "e_blob"),  &la);
    const uint8_t *gb = basta_get_blob(basta_map_get(back, "i_blob2"), &lb);
    CHECK(la == sizeof(payload) && memcmp(ga, payload, la) == 0);
    CHECK(lb == sizeof(payload) && memcmp(gb, payload, lb) == 0);

    /* number and bool roundtripped */
    CHECK(basta_get_bool(basta_map_get(back, "b_bool")) == 1);
    CHECK(basta_get_number(basta_map_get(back, "c_number")) == 3.14);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  19. Nested map containing blobs                                    */
/* ------------------------------------------------------------------ */

static void test_nested_map_blobs(void) {
    SECTION("nested map containing blobs");

    uint8_t vdata[] = {0x01, 0x02, 0x03};
    uint8_t fdata[] = {0xA0, 0xB0};

    BastaValue *root = basta_new_map();

    BastaValue *shaders = basta_new_map();
    BastaValue *vert = basta_new_map();
    basta_set(vert, "stage",  basta_new_string("vertex"));
    basta_set(vert, "spirv",  basta_new_blob(vdata, sizeof(vdata)));
    BastaValue *frag = basta_new_map();
    basta_set(frag, "stage",  basta_new_string("fragment"));
    basta_set(frag, "spirv",  basta_new_blob(fdata, sizeof(fdata)));
    basta_set(shaders, "vert", vert);
    basta_set(shaders, "frag", frag);
    basta_set(root, "shaders", shaders);
    basta_set(root, "version", basta_new_number(1));

    size_t doc_len;
    char *buf = basta_write(root, BASTA_PRETTY, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    const BastaValue *bshaders = basta_map_get(back, "shaders");
    const BastaValue *bvert    = basta_map_get(bshaders, "vert");
    const BastaValue *bfrag    = basta_map_get(bshaders, "frag");

    CHECK(strcmp(basta_get_string(basta_map_get(bvert, "stage")), "vertex") == 0);
    CHECK(strcmp(basta_get_string(basta_map_get(bfrag, "stage")), "fragment") == 0);

    size_t vl, fl;
    const uint8_t *gv = basta_get_blob(basta_map_get(bvert, "spirv"), &vl);
    const uint8_t *gf = basta_get_blob(basta_map_get(bfrag, "spirv"), &fl);
    CHECK(vl == sizeof(vdata) && memcmp(gv, vdata, vl) == 0);
    CHECK(fl == sizeof(fdata) && memcmp(gf, fdata, fl) == 0);

    free(buf);
    basta_free(root);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  20. Blob followed immediately by a blob (adjacent in array)       */
/* ------------------------------------------------------------------ */

static void test_adjacent_blobs(void) {
    SECTION("adjacent blobs in array");

    /* Array of 100 single-byte blobs — densely packed sentinels */
    BastaValue *arr = basta_new_array();
    for (int i = 0; i < 100; i++) {
        uint8_t b = (uint8_t)i;
        basta_push(arr, basta_new_blob(&b, 1));
    }

    size_t doc_len;
    char *buf = basta_write(arr, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_count(back) == 100);

    int all_ok = 1;
    for (int i = 0; i < 100; i++) {
        size_t gl;
        const uint8_t *got = basta_get_blob(basta_array_get(back, (size_t)i), &gl);
        if (!got || gl != 1 || got[0] != (uint8_t)i) { all_ok = 0; break; }
    }
    CHECK(all_ok);

    free(buf);
    basta_free(arr);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  21. Blob with all-0xFF payload                                     */
/* ------------------------------------------------------------------ */

static void test_blob_all_ff(void) {
    SECTION("blob with all-0xFF payload");

    size_t sz = 512;
    uint8_t *data = (uint8_t *)malloc(sz);
    memset(data, 0xFF, sz);

    BastaValue *v = basta_new_blob(data, sz);
    free(data);

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_type(back) == BASTA_BLOB);

    size_t gl;
    const uint8_t *got = basta_get_blob(back, &gl);
    CHECK(gl == sz);
    int all_ff = 1;
    for (size_t i = 0; i < gl; i++) if (got[i] != 0xFF) { all_ff = 0; break; }
    CHECK(all_ff);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  22. All-0x00 payload (blobs within blobs, in effect)              */
/* ------------------------------------------------------------------ */

static void test_blob_all_zeros(void) {
    SECTION("blob with all-0x00 payload");

    size_t sz = 256;
    uint8_t *data = (uint8_t *)calloc(sz, 1);

    BastaValue *v = basta_new_map();
    basta_set(v, "zeros", basta_new_blob(data, sz));
    basta_set(v, "after", basta_new_string("still here"));
    free(data);

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    size_t gl;
    const uint8_t *got = basta_get_blob(basta_map_get(back, "zeros"), &gl);
    CHECK(gl == sz);
    int all_zero = 1;
    for (size_t i = 0; i < gl; i++) if (got[i] != 0) { all_zero = 0; break; }
    CHECK(all_zero);

    /* Crucially: the key after the blob must still be readable */
    CHECK(strcmp(basta_get_string(basta_map_get(back, "after")), "still here") == 0);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  23. Blob as top-level value (not inside a container)              */
/* ------------------------------------------------------------------ */

static void test_blob_toplevel(void) {
    SECTION("blob as top-level value");

    uint8_t data[] = {0x11, 0x22, 0x33};
    BastaValue *v = basta_new_blob(data, sizeof(data));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);
    CHECK(basta_type(back) == BASTA_BLOB);

    size_t gl;
    const uint8_t *got = basta_get_blob(back, &gl);
    CHECK(gl == sizeof(data) && memcmp(got, data, gl) == 0);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  24. Multiline string adjacent to blob — no cross-contamination    */
/* ------------------------------------------------------------------ */

static void test_multiline_string_adjacent_to_blob(void) {
    SECTION("multiline string adjacent to blob");

    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

    BastaValue *v = basta_new_map();
    basta_set(v, "script", basta_new_string("line1\nline2\nline3"));
    basta_set(v, "data",   basta_new_blob(payload, sizeof(payload)));
    basta_set(v, "more",   basta_new_string("another\nmultiline\nvalue"));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_PRETTY, &doc_len);
    CHECK(buf != NULL);

    BastaResult r;
    BastaValue *back = basta_parse(buf, doc_len, &r);
    CHECK(back != NULL);
    CHECK(r.code == BASTA_OK);

    CHECK(strcmp(basta_get_string(basta_map_get(back, "script")), "line1\nline2\nline3") == 0);
    CHECK(strcmp(basta_get_string(basta_map_get(back, "more")),   "another\nmultiline\nvalue") == 0);

    size_t gl;
    const uint8_t *got = basta_get_blob(basta_map_get(back, "data"), &gl);
    CHECK(gl == sizeof(payload) && memcmp(got, payload, gl) == 0);

    free(buf);
    basta_free(v);
    basta_free(back);
}

/* ------------------------------------------------------------------ */
/*  25. basta_write with NULL out_len (no crash)                       */
/* ------------------------------------------------------------------ */

static void test_write_null_out_len(void) {
    SECTION("basta_write with NULL out_len");

    uint8_t d[] = {0x01, 0x02};
    BastaValue *v = basta_new_map();
    basta_set(v, "d", basta_new_blob(d, sizeof(d)));

    char *buf = basta_write(v, BASTA_COMPACT, NULL);
    CHECK(buf != NULL);
    /* We cannot safely use strlen here, but at minimum it must not crash */

    free(buf);
    basta_free(v);
}

/* ------------------------------------------------------------------ */
/*  26. Error: stray 0x00 in key position                             */
/* ------------------------------------------------------------------ */

static void test_error_blob_in_key_position(void) {
    SECTION("error: blob sentinel in key position");

    /* A map where the key slot contains a 0x00 byte.
       Wire: { 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 : null }
       (sentinel + 8 zero size bytes = zero-length blob, then ':')
       This is syntactically invalid: a blob cannot be a map key. */
    char buf[32];
    int pos = 0;
    buf[pos++] = '{';
    buf[pos++] = 0x00;                       /* sentinel */
    for (int i = 0; i < 8; i++) buf[pos++] = 0x00;  /* size = 0 */
    /* no data bytes for zero-length blob */
    memcpy(buf + pos, ": null}", 7); pos += 7;

    BastaResult r;
    BastaValue *v = basta_parse(buf, (size_t)pos, &r);
    /* The parser must reject this — a blob is not a valid map key */
    CHECK(v == NULL);
    CHECK(r.code != BASTA_OK);
}

/* ------------------------------------------------------------------ */
/*  27. COMPACT vs PRETTY output both parse to identical trees        */
/* ------------------------------------------------------------------ */

static void test_compact_pretty_equivalence(void) {
    SECTION("COMPACT and PRETTY produce equivalent parse trees");

    uint8_t p1[] = {0xAA, 0xBB, 0xCC};
    uint8_t p2[] = {0x11};

    BastaValue *orig = basta_new_map();
    basta_set(orig, "name",   basta_new_string("test"));
    basta_set(orig, "blob1",  basta_new_blob(p1, sizeof(p1)));
    basta_set(orig, "count",  basta_new_number(42));
    basta_set(orig, "blob2",  basta_new_blob(p2, sizeof(p2)));

    size_t clen, plen;
    char *cbuf = basta_write(orig, BASTA_COMPACT, &clen);
    char *pbuf = basta_write(orig, BASTA_PRETTY,  &plen);
    CHECK(cbuf != NULL && pbuf != NULL);

    BastaResult rc, rp;
    BastaValue *from_compact = basta_parse(cbuf, clen, &rc);
    BastaValue *from_pretty  = basta_parse(pbuf, plen, &rp);
    CHECK(from_compact != NULL && rc.code == BASTA_OK);
    CHECK(from_pretty  != NULL && rp.code == BASTA_OK);

    /* Both trees must yield identical blobs */
    size_t ca, pa, cb2, pb2;
    const uint8_t *gca = basta_get_blob(basta_map_get(from_compact, "blob1"), &ca);
    const uint8_t *gpa = basta_get_blob(basta_map_get(from_pretty,  "blob1"), &pa);
    CHECK(ca == sizeof(p1) && pa == sizeof(p1));
    CHECK(memcmp(gca, p1, ca) == 0 && memcmp(gpa, p1, pa) == 0);

    const uint8_t *gcb = basta_get_blob(basta_map_get(from_compact, "blob2"), &cb2);
    const uint8_t *gpb = basta_get_blob(basta_map_get(from_pretty,  "blob2"), &pb2);
    CHECK(cb2 == sizeof(p2) && pb2 == sizeof(p2));
    CHECK(gcb[0] == 0x11 && gpb[0] == 0x11);

    free(cbuf); free(pbuf);
    basta_free(orig);
    basta_free(from_compact);
    basta_free(from_pretty);
}

/* ------------------------------------------------------------------ */
/*  28. Wire encoding byte-level sanity                               */
/* ------------------------------------------------------------------ */

static void test_wire_encoding(void) {
    SECTION("wire encoding byte-level sanity");

    uint8_t data[] = {0xAB, 0xCD};
    BastaValue *v  = basta_new_blob(data, sizeof(data));

    size_t doc_len;
    char *buf = basta_write(v, BASTA_COMPACT, &doc_len);
    CHECK(buf != NULL);
    /* Top-level blob: wire is exactly 1 + 8 + 2 = 11 bytes (plus trailing \n) */
    CHECK(doc_len >= 11);

    unsigned char *w = (unsigned char *)buf;
    CHECK(w[0] == 0x00);                      /* sentinel */
    CHECK(w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0);  /* high 4 BE bytes = 0 */
    CHECK(w[5] == 0 && w[6] == 0 && w[7] == 0);                /* next 3 BE bytes = 0 */
    CHECK(w[8] == 2);                          /* low byte of size = 2 */
    CHECK(w[9] == 0xAB && w[10] == 0xCD);      /* payload bytes */

    free(buf);
    basta_free(v);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("Basta test suite\n");

    test_text_only();
    test_blob_value();
    test_blob_in_map();
    test_blob_in_array();
    test_roundtrip();
    test_parse_blob();
    test_parse_multiple_blobs();
    test_empty_blob();
    test_blob_containing_zeros();
    test_sections_with_blobs();
    test_error_truncated_size();
    test_error_truncated_data();
    test_sorted_flag();
    test_out_len();
    test_map_of_blobs();
    test_array_of_blobs();
    test_large_blob();
    test_blob_interleaved_types();
    test_nested_map_blobs();
    test_adjacent_blobs();
    test_blob_all_ff();
    test_blob_all_zeros();
    test_blob_toplevel();
    test_multiline_string_adjacent_to_blob();
    test_write_null_out_len();
    test_error_blob_in_key_position();
    test_compact_pretty_equivalence();
    test_wire_encoding();

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
