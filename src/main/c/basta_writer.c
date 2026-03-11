#include "basta_internal.h"
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Dynamic byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_init(Buf *b) {
    b->cap  = 256;
    b->len  = 0;
    b->data = (char *)malloc(b->cap);
    return b->data ? 0 : -1;
}

static int buf_grow(Buf *b, size_t need) {
    if (b->len + need < b->cap) return 0;
    size_t new_cap = b->cap * 2;
    while (new_cap < b->len + need) new_cap *= 2;
    char *tmp = (char *)realloc(b->data, new_cap);
    if (!tmp) return -1;
    b->data = tmp;
    b->cap  = new_cap;
    return 0;
}

/* Append n raw bytes — safe for binary (no null-termination side effects). */
static int buf_append(Buf *b, const char *s, size_t n) {
    if (buf_grow(b, n + 1)) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0'; /* keep the buffer null-terminated past the end */
    return 0;
}

static int buf_puts(Buf *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

static int buf_putc(Buf *b, char c) {
    return buf_append(b, &c, 1);
}

static int buf_putb(Buf *b, unsigned char byte) {
    char c = (char)byte;
    return buf_append(b, &c, 1);
}

/* ------------------------------------------------------------------ */
/*  Indent helper                                                      */
/* ------------------------------------------------------------------ */

static int buf_indent(Buf *b, int depth) {
    for (int i = 0; i < depth; i++)
        if (buf_puts(b, "  ")) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Label writing                                                      */
/* ------------------------------------------------------------------ */

static int is_label_symbol(char c) {
    return c == '!' || c == '#' || c == '$' || c == '%'
        || c == '&' || c == '.' || c == '_';
}

static int is_label_char(char c) {
    return isalnum((unsigned char)c) || is_label_symbol(c);
}

static int is_keyword(const char *s, size_t len) {
    return (len == 4 && memcmp(s, "true",  4) == 0)
        || (len == 5 && memcmp(s, "false", 5) == 0)
        || (len == 4 && memcmp(s, "null",  4) == 0)
        || (len == 3 && memcmp(s, "Inf",   3) == 0)
        || (len == 3 && memcmp(s, "NaN",   3) == 0);
}

static int is_bare_label(const char *s) {
    size_t len = strlen(s);
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++)
        if (!is_label_char(s[i])) return 0;
    if (is_keyword(s, len)) return 0;
    return 1;
}

static int write_label(Buf *b, const char *key) {
    if (is_bare_label(key)) return buf_puts(b, key);
    if (buf_putc(b, '"'))   return -1;
    if (buf_puts(b, key))   return -1;
    if (buf_putc(b, '"'))   return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  String writing                                                     */
/* ------------------------------------------------------------------ */

static int has_newline(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] == '\n' || s[i] == '\r') return 1;
    return 0;
}

static int write_string(Buf *b, const char *s, size_t len) {
    if (has_newline(s, len)) {
        if (buf_puts(b, "\"\"\""))     return -1;
        if (buf_append(b, s, len))     return -1;
        if (buf_puts(b, "\"\"\""))     return -1;
    } else {
        if (buf_putc(b, '"'))          return -1;
        if (buf_append(b, s, len))     return -1;
        if (buf_putc(b, '"'))          return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Number formatting                                                  */
/* ------------------------------------------------------------------ */

static int write_number(Buf *b, double n) {
    char tmp[64];
    if (isnan(n))  return buf_puts(b, "NaN");
    if (isinf(n))  return buf_puts(b, n < 0 ? "-Inf" : "Inf");
    if (n == (long long)n && n >= -1e15 && n <= 1e15)
        snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
    else
        snprintf(tmp, sizeof(tmp), "%.17g", n);
    return buf_puts(b, tmp);
}

/* ------------------------------------------------------------------ */
/*  Blob writing                                                       */
/* ------------------------------------------------------------------ */

/* Wire format: 0x00 | uint64be(N) | N bytes */
static int write_blob(Buf *b, const uint8_t *data, size_t len) {
    /* sentinel */
    if (buf_putb(b, 0x00)) return -1;

    /* 8-byte big-endian length */
    uint64_t n = (uint64_t)len;
    for (int i = 7; i >= 0; i--) {
        if (buf_putb(b, (unsigned char)((n >> (i * 8)) & 0xFF))) return -1;
    }

    /* payload */
    return buf_append(b, (const char *)data, len);
}

/* ------------------------------------------------------------------ */
/*  Sorted index helper                                                */
/* ------------------------------------------------------------------ */

static size_t *sorted_indices(const BastaMember *items, size_t count) {
    size_t *idx = (size_t *)malloc(count * sizeof(size_t));
    if (!idx) return NULL;
    for (size_t i = 0; i < count; i++) idx[i] = i;
    for (size_t i = 1; i < count; i++) {
        size_t tmp = idx[i], j = i;
        while (j > 0 && strcmp(items[idx[j-1]].key, items[tmp].key) > 0) {
            idx[j] = idx[j-1]; j--;
        }
        idx[j] = tmp;
    }
    return idx;
}

/* ------------------------------------------------------------------ */
/*  Recursive writer                                                   */
/* ------------------------------------------------------------------ */

static int write_value(Buf *b, const BastaValue *v, int compact, int sorted, int depth);

static int write_array(Buf *b, const BastaValue *v, int compact, int sorted, int depth) {
    size_t count = v->as.array.count;
    if (count == 0) return buf_puts(b, "[]");

    if (buf_putc(b, '[')) return -1;
    for (size_t i = 0; i < count; i++) {
        if (compact) {
            if (i > 0 && buf_puts(b, ", ")) return -1;
        } else {
            if (buf_putc(b, '\n'))           return -1;
            if (buf_indent(b, depth + 1))    return -1;
        }
        if (write_value(b, v->as.array.items[i], compact, sorted, depth + 1)) return -1;
        if (!compact && i + 1 < count)
            if (buf_putc(b, ',')) return -1;
    }
    if (!compact) {
        if (buf_putc(b, '\n'))        return -1;
        if (buf_indent(b, depth))     return -1;
    }
    return buf_putc(b, ']');
}

static int write_map(Buf *b, const BastaValue *v, int compact, int sorted, int depth) {
    size_t count = v->as.map.count;
    if (count == 0) return buf_puts(b, "{}");

    size_t *order = NULL;
    if (sorted && count > 1) {
        order = sorted_indices(v->as.map.items, count);
        if (!order) return -1;
    }

    if (buf_putc(b, '{')) { free(order); return -1; }
    for (size_t n = 0; n < count; n++) {
        size_t i = order ? order[n] : n;
        if (compact) {
            if (n > 0 && buf_puts(b, ", ")) { free(order); return -1; }
        } else {
            if (buf_putc(b, '\n'))           { free(order); return -1; }
            if (buf_indent(b, depth + 1))    { free(order); return -1; }
        }
        if (write_label(b, v->as.map.items[i].key))                                          { free(order); return -1; }
        if (buf_puts(b, ": "))                                                                { free(order); return -1; }
        if (write_value(b, v->as.map.items[i].value, compact, sorted, depth + 1))           { free(order); return -1; }
        if (!compact && n + 1 < count && buf_putc(b, ','))                                   { free(order); return -1; }
    }
    free(order);
    if (!compact) {
        if (buf_putc(b, '\n'))    return -1;
        if (buf_indent(b, depth)) return -1;
    }
    return buf_putc(b, '}');
}

static int write_value(Buf *b, const BastaValue *v, int compact, int sorted, int depth) {
    if (!v) return buf_puts(b, "null");
    switch (v->type) {
    case BASTA_NULL:   return buf_puts(b, "null");
    case BASTA_BOOL:   return buf_puts(b, v->as.boolean ? "true" : "false");
    case BASTA_NUMBER: return write_number(b, v->as.number);
    case BASTA_STRING: return write_string(b, v->as.string.data, v->as.string.len);
    case BASTA_LABEL:  return buf_append(b, v->as.string.data, v->as.string.len);
    case BASTA_ARRAY:  return write_array(b, v, compact, sorted, depth);
    case BASTA_MAP:    return write_map(b, v, compact, sorted, depth);
    case BASTA_BLOB:   return write_blob(b, v->as.blob.data, v->as.blob.len);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Section writer                                                     */
/* ------------------------------------------------------------------ */

static int write_sections(Buf *b, const BastaValue *v, int compact, int sorted) {
    if (!v || v->type != BASTA_MAP) return write_value(b, v, compact, sorted, 0);

    size_t count = v->as.map.count;
    size_t *order = NULL;
    if (sorted && count > 1) {
        order = sorted_indices(v->as.map.items, count);
        if (!order) return -1;
    }

    for (size_t n = 0; n < count; n++) {
        size_t i = order ? order[n] : n;
        if (n > 0 && buf_putc(b, '\n'))                                { free(order); return -1; }
        if (buf_putc(b, '@'))                                          { free(order); return -1; }
        if (write_label(b, v->as.map.items[i].key))                    { free(order); return -1; }
        if (compact ? buf_putc(b, ' ') : buf_putc(b, '\n'))            { free(order); return -1; }
        if (write_value(b, v->as.map.items[i].value, compact, sorted, 0)) { free(order); return -1; }
        if (!compact && buf_putc(b, '\n'))                             { free(order); return -1; }
    }
    free(order);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

BASTA_API char *basta_write(const BastaValue *v, int flags, size_t *out_len) {
    Buf b;
    if (buf_init(&b)) return NULL;

    int compact  = (flags & BASTA_COMPACT)  != 0;
    int sections = (flags & BASTA_SECTIONS) != 0;
    int sorted   = (flags & BASTA_SORTED)   != 0;

    int err = sections
        ? write_sections(&b, v, compact, sorted)
        : write_value(&b, v, compact, sorted, 0);

    if (err) { free(b.data); return NULL; }

    /* Ensure trailing newline for pretty text output.
       Skip this if the document ends with binary blob data (last byte
       is not a printable text character). */
    if (!compact && b.len > 0 && b.data[b.len - 1] != '\n') {
        buf_putc(&b, '\n');
    }

    if (out_len) *out_len = b.len;
    return b.data;
}

BASTA_API int basta_write_fp(const BastaValue *v, int flags, void *fp) {
    size_t len;
    char *s = basta_write(v, flags, &len);
    if (!s) return -1;
    int ret = fwrite(s, 1, len, (FILE *)fp) == len ? 0 : -1;
    free(s);
    return ret;
}
