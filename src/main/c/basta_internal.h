#ifndef BASTA_INTERNAL_H
#define BASTA_INTERNAL_H

#include "basta.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- Value representation ---- */

typedef struct BastaMember {
    char       *key;
    BastaValue *value;
} BastaMember;

struct BastaValue {
    BastaType type;
    uint8_t   num_fmt;   /* BASTA_NUM_DEC / _HEX / _BIN (0 for non-numbers) */
    union {
        int     boolean;
        double  number;
        struct { char    *data; size_t len; } string;   /* STRING and LABEL */
        struct { uint8_t *data; size_t len; } blob;     /* BLOB */
        struct { BastaValue **items; size_t count; size_t cap; } array;
        struct { BastaMember *items;  size_t count; size_t cap; } map;
    } as;
};

/* ---- Value constructors (internal) ---- */

BastaValue *basta_value_null(void);
BastaValue *basta_value_bool(int b);
BastaValue *basta_value_number(double n);
BastaValue *basta_value_number_fmt(double n, uint8_t fmt);
BastaValue *basta_value_string(const char *s, size_t len);
BastaValue *basta_value_label(const char *s, size_t len);
BastaValue *basta_value_blob(const uint8_t *data, size_t len);
BastaValue *basta_value_array(void);
BastaValue *basta_value_map(void);

int basta_array_push(BastaValue *arr, BastaValue *item);
int basta_map_put(BastaValue *map, const char *key, size_t key_len, BastaValue *value);

/* ---- Lexer ---- */

typedef enum {
    TOK_LBRACE,      /* { */
    TOK_RBRACE,      /* } */
    TOK_LBRACKET,    /* [ */
    TOK_RBRACKET,    /* ] */
    TOK_COLON,       /* : */
    TOK_COMMA,       /* , */
    TOK_STRING,
    TOK_MSTRING,     /* multiline """...""" */
    TOK_NUMBER,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_LABEL,
    TOK_AT,          /* @ (section marker) */
    TOK_BLOB,        /* 0x00 <8-byte BE size> <data> */
    TOK_EOF,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType   type;
    const char *start;   /* pointer into source buffer */
    size_t      len;     /* byte count of the full token in the source buffer */
    int         line;
    int         col;
} Token;

typedef struct {
    const char *src;
    size_t      src_len;
    size_t      pos;
    int         line;
    int         col;
} Lexer;

void  lexer_init(Lexer *lex, const char *src, size_t len);
Token lexer_next(Lexer *lex);

#endif /* BASTA_INTERNAL_H */
