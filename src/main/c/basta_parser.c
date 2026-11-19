#include "basta_internal.h"
#include <stdio.h>
#include <ctype.h>

typedef struct {
    Lexer       lex;
    Token       current;
    BastaResult result;
    int         had_error;
} Parser;

static void parser_init(Parser *p, const char *src, size_t len) {
    basta_lexer_init(&p->lex, src, len);
    p->had_error    = 0;
    p->result.code     = BASTA_OK;
    p->result.line     = 0;
    p->result.col      = 0;
    p->result.sections = 0;
    p->result.message[0] = '\0';
    p->current = basta_lexer_next(&p->lex);
}

static void parser_error_code(Parser *p, BastaError code, const char *msg) {
    if (p->had_error) return;
    p->had_error    = 1;
    p->result.code  = code;
    p->result.line  = p->current.line;
    p->result.col   = p->current.col;
    snprintf(p->result.message, sizeof(p->result.message), "%s", msg);
}

static void parser_error(Parser *p, const char *msg) {
    parser_error_code(p, BASTA_ERR_SYNTAX, msg);
}

static void advance(Parser *p) {
    p->current = basta_lexer_next(&p->lex);
    if (p->current.type == TOK_ERROR) {
        /* propagate lexer errors: truncated blob becomes BASTA_ERR_BLOB_TRUNCATED */
        const char *msg = p->current.start;
        BastaError code = BASTA_ERR_SYNTAX;
        if (strstr(msg, "truncated blob"))
            code = BASTA_ERR_BLOB_TRUNCATED;
        parser_error_code(p, code, msg);
    }
}

static int check(const Parser *p, TokenType type) {
    return p->current.type == type;
}

static int expect(Parser *p, TokenType type, const char *msg) {
    if (check(p, type)) { advance(p); return 1; }
    parser_error(p, msg);
    return 0;
}

static int label_symbol_char(char c) {
    return c == '!' || c == '#' || c == '$' || c == '%'
        || c == '&' || c == '.' || c == '_';
}

/* A number- or keyword-spelled token is, in key / section-name position, a
   valid unquoted-label provided every byte of its lexeme is a labelchar.
   This is what lets { 0: 1 }, { true: false } and @0 { } parse: the grammar's
   key and section-name productions are `label`, and 0 / true are valid
   unquoted-labels.  Signed or otherwise non-label spellings such as -5 or
   -Inf contain '-', which is not a labelchar, so they are rejected.
   (Blobs are never keys: a BASTA_BLOB has no TOK_LABEL/NUMBER lexeme.) */
static int token_is_unquoted_label(const Token *t) {
    if (t->len == 0) return 0;
    for (size_t i = 0; i < t->len; i++) {
        unsigned char c = (unsigned char)t->start[i];
        if (!isalnum(c) && !label_symbol_char((char)c)) return 0;
    }
    return 1;
}

/* Is this token usable as a map key or section name?
   (unquoted-label | quoted-label, per the grammar) */
static int is_key_token(const Parser *p) {
    switch (p->current.type) {
        case TOK_LABEL:
        case TOK_STRING:
            return 1;
        case TOK_NUMBER:
        case TOK_TRUE:
        case TOK_FALSE:
        case TOK_NULL:
            return token_is_unquoted_label(&p->current);
        default:
            return 0;
    }
}

static BastaValue *parse_value(Parser *p);

/* ------------------------------------------------------------------ */
/*  Literal decoders                                                   */
/* ------------------------------------------------------------------ */

/* An integer literal past the signed/unsigned 64-bit range is still a valid
   number by the grammar, so it is read rather than rejected: as the nearest
   double, of kind BASTA_NUMBER_FLOAT.  The kind is the exactness flag.  The
   checked integer getters refuse a FLOAT, so a consumer asking for an exact
   value gets a failure, never a rounded one; the legacy double getter returns
   the nearest value, as it always has. */
static BastaValue *parse_wide_integer(Parser *parser, const char *start,
                                      size_t len, size_t first, int base,
                                      uint8_t fmt, int negative) {
    double number;
    if (base == 10) {
        /* strtod rounds decimal correctly, and reads the sign itself. */
        char *text = (char *)malloc(len + 1);
        if (!text) {
            parser_error_code(parser, BASTA_ERR_ALLOC, "allocation failed");
            return NULL;
        }
        memcpy(text, start, len);
        text[len] = '\0';
        number = strtod(text, NULL);
        free(text);
    } else {
        /* Hex and binary: keep the leading 61+ bits and fold every dropped
           nonzero bit into the lowest one (a sticky bit), so converting that
           head to double rounds exactly as the full value would.  This avoids
           depending on strtod reading hex, which older C runtimes cannot. */
        unsigned bits = base == 16 ? 4 : 1;
        uint64_t head = 0;
        int shift = 0, sticky = 0;
        for (size_t i = first; i < len; i++) {
            unsigned char c = (unsigned char)start[i];
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                           : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                           : (unsigned)(c - 'A' + 10);
            if ((head >> (64 - bits)) == 0) {
                head = (head << bits) | digit;
            } else {
                if (shift < 4096) shift += (int)bits;   /* past this, it is Inf */
                sticky |= digit != 0;
            }
        }
        number = ldexp((double)(head | (uint64_t)sticky), shift);
        if (negative) number = -number;
    }
    return basta_value_number_fmt(number, fmt);
}

static BastaValue *parse_number_literal
(
    Parser *parser
)
{
    const char *start = parser->current.start;
    size_t len = parser->current.len, at = start[0] == '-' ? 1 : 0;
    int negative = at != 0, base = 10;
    uint8_t fmt = BASTA_NUM_DEC;
    if (len - at >= 3 && start[at] == '0')
    {
        if (start[at + 1] == 'x' || start[at + 1] == 'X') { base = 16; fmt = BASTA_NUM_HEX; at += 2; }
        else if (start[at + 1] == 'b' || start[at + 1] == 'B') { base = 2; fmt = BASTA_NUM_BIN; at += 2; }
    }
    if (base == 10 && (start[at] == 'I' || start[at] == 'N'
        || memchr(start, '.', len) || memchr(start, 'e', len) || memchr(start, 'E', len)))
    {
        /* Float tokens have no fixed text-length ceiling or silent truncation. */
        if (len == SIZE_MAX) { parser_error_code(parser, BASTA_ERR_ALLOC, "number allocation overflow"); return NULL; }
        char *text = (char *)malloc(len + 1);
        if (!text) { parser_error_code(parser, BASTA_ERR_ALLOC, "allocation failed"); return NULL; }
        memcpy(text, start, len); text[len] = 0;
        double number = strtod(text, NULL);
        free(text);
        return basta_value_number_fmt(number, fmt);
    }
    uint64_t magnitude = 0;
    uint64_t limit = negative ? (UINT64_C(1) << 63) : UINT64_MAX;
    size_t first = at;
    for (; at < len; at++)
    {
        unsigned char c = (unsigned char)start[at];
        unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
        if (digit >= (unsigned)base)
        {
            /* The lexer only emits well-formed numbers; this is a guard. */
            parser_error_code(parser, BASTA_ERR_SYNTAX, "invalid digit in number");
            return NULL;
        }
        if (magnitude > (limit - digit) / (unsigned)base)
            return parse_wide_integer(parser, start, len, first, base, fmt, negative);
        magnitude = magnitude * (unsigned)base + digit;
    }
    if (negative)
    {
        int64_t integer = magnitude == (UINT64_C(1) << 63) ? INT64_MIN : -(int64_t)magnitude;
        return basta_new_int64_fmt(integer, fmt);
    }
    return magnitude <= INT64_MAX ? basta_new_int64_fmt((int64_t)magnitude, fmt) : basta_new_uint64_fmt(magnitude, fmt);
}

static BastaValue *extract_string(const char *start, size_t len) {
    return basta_value_string(start + 1, len - 2);
}

static BastaValue *extract_mstring(const char *start, size_t len) {
    return basta_value_string(start + 3, len - 6);
}

static void extract_key(const Token *tok, const char **key, size_t *key_len) {
    if (tok->type == TOK_STRING) {
        *key     = tok->start + 1;
        *key_len = tok->len - 2;
    } else {
        *key     = tok->start;
        *key_len = tok->len;
    }
}

/* Decode a TOK_BLOB token: wire format is 0x00 | uint64be(N) | N bytes.
   The token's start points at the 0x00 sentinel; total len = 1+8+N. */
static BastaValue *extract_blob(const char *start, size_t total_len) {
    /* sanity: must have at least sentinel + 8-byte size */
    if (total_len < 9) return NULL;

    const unsigned char *p = (const unsigned char *)start;

    /* p[0] == 0x00 (sentinel, already verified by lexer) */
    uint64_t blob_len = 0;
    for (int i = 0; i < 8; i++)
        blob_len = (blob_len << 8) | p[1 + i];

    /* total_len must equal 1 + 8 + blob_len */
    if (total_len != (size_t)(9 + blob_len)) return NULL;

    return basta_value_blob(p + 9, (size_t)blob_len);
}

/* ------------------------------------------------------------------ */
/*  Container parsers                                                  */
/* ------------------------------------------------------------------ */

static BastaValue *parse_array(Parser *p) {
    advance(p); /* consume [ */
    BastaValue *arr = basta_value_array();
    if (!arr) { parser_error(p, "allocation failed"); return NULL; }

    if (!check(p, TOK_RBRACKET)) {
        BastaValue *elem = parse_value(p);
        if (!elem || p->had_error) { basta_free(arr); basta_free(elem); return NULL; }
        basta_array_push(arr, elem);

        while (check(p, TOK_COMMA)) {
            advance(p);
            elem = parse_value(p);
            if (!elem || p->had_error) { basta_free(arr); basta_free(elem); return NULL; }
            basta_array_push(arr, elem);
        }
    }

    if (!expect(p, TOK_RBRACKET, "expected ']'")) { basta_free(arr); return NULL; }
    return arr;
}

static BastaValue *parse_map(Parser *p) {
    advance(p); /* consume { */
    BastaValue *map = basta_value_map();
    if (!map) { parser_error(p, "allocation failed"); return NULL; }

    if (!check(p, TOK_RBRACE)) {
        if (!is_key_token(p)) {
            parser_error(p, "expected label or quoted key in map");
            basta_free(map); return NULL;
        }
        Token key_tok = p->current;
        advance(p);
        if (!expect(p, TOK_COLON, "expected ':' after key")) { basta_free(map); return NULL; }
        BastaValue *val = parse_value(p);
        if (!val || p->had_error) { basta_free(map); basta_free(val); return NULL; }
        const char *k; size_t klen;
        extract_key(&key_tok, &k, &klen);
        basta_map_put(map, k, klen, val);

        while (check(p, TOK_COMMA)) {
            advance(p);
            if (!is_key_token(p)) {
                parser_error(p, "expected label or quoted key in map");
                basta_free(map); return NULL;
            }
            key_tok = p->current;
            advance(p);
            if (!expect(p, TOK_COLON, "expected ':' after key")) { basta_free(map); return NULL; }
            val = parse_value(p);
            if (!val || p->had_error) { basta_free(map); basta_free(val); return NULL; }
            extract_key(&key_tok, &k, &klen);
            basta_map_put(map, k, klen, val);
        }
    }

    if (!expect(p, TOK_RBRACE, "expected '}'")) { basta_free(map); return NULL; }
    return map;
}

/* ------------------------------------------------------------------ */
/*  Value parser — the only place blob is dispatched                  */
/* ------------------------------------------------------------------ */

static BastaValue *parse_value(Parser *p) {
    if (p->had_error) return NULL;

    switch (p->current.type) {
        case TOK_LBRACE:   return parse_map(p);
        case TOK_LBRACKET: return parse_array(p);
        case TOK_STRING: {
            BastaValue *v = extract_string(p->current.start, p->current.len);
            if (!v) parser_error(p, "allocation failed");
            advance(p);
            return v;
        }
        case TOK_MSTRING: {
            BastaValue *v = extract_mstring(p->current.start, p->current.len);
            if (!v) parser_error(p, "allocation failed");
            advance(p);
            return v;
        }
        case TOK_NUMBER: {
            BastaValue *v = parse_number_literal(p);
            if (!v) { if (!p->had_error) parser_error_code(p, BASTA_ERR_ALLOC, "allocation failed"); return NULL; }
            advance(p);
            return v;
        }
        case TOK_TRUE:  advance(p); return basta_value_bool(1);
        case TOK_FALSE: advance(p); return basta_value_bool(0);
        case TOK_NULL:  advance(p); return basta_value_null();
        case TOK_LABEL: {
            BastaValue *v = basta_value_label(p->current.start, p->current.len);
            if (!v) parser_error(p, "allocation failed");
            advance(p);
            return v;
        }
        case TOK_BLOB: {
            /* 0x00 was in value-start position: decode the wire encoding */
            BastaValue *v = extract_blob(p->current.start, p->current.len);
            if (!v) parser_error(p, "allocation failed");
            advance(p);
            return v;
        }
        case TOK_EOF:
            parser_error_code(p, BASTA_ERR_UNEXPECTED_EOF, "unexpected end of input");
            return NULL;
        default:
            parser_error_code(p, BASTA_ERR_UNEXPECTED_TOKEN, "unexpected token");
            return NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Top-level: section vs container-seq logic (identical to Pasta)    */
/* ------------------------------------------------------------------ */

static int is_container_start(const Parser *p) {
    return check(p, TOK_LBRACE) || check(p, TOK_LBRACKET);
}

static void extract_section_name(const Token *tok, const char **name, size_t *len) {
    if (tok->type == TOK_STRING) { *name = tok->start + 1; *len = tok->len - 2; }
    else                         { *name = tok->start;     *len = tok->len;     }
}

/* ---- Public parse API ---- */

BASTA_API BastaValue *basta_parse(const char *input, size_t len, BastaResult *result) {
    Parser p;
    parser_init(&p, input, len);

    if (check(&p, TOK_AT)) {
        p.result.sections = 1;
        BastaValue *map = basta_value_map();
        if (!map) { parser_error(&p, "allocation failed"); if (result) *result = p.result; return NULL; }

        while (!p.had_error && check(&p, TOK_AT)) {
            advance(&p);
            /* Section name is a `label`, so number-/keyword-spelled names
               like @0 are valid. */
            if (!is_key_token(&p)) {
                parser_error(&p, "expected section name after '@'");
                basta_free(map); if (result) *result = p.result; return NULL;
            }
            Token name_tok = p.current;
            advance(&p);
            if (!is_container_start(&p)) {
                parser_error(&p, "expected container (map or array) after section name");
                basta_free(map); if (result) *result = p.result; return NULL;
            }
            BastaValue *container = parse_value(&p);
            if (!container || p.had_error) {
                basta_free(map); basta_free(container);
                if (result) *result = p.result;
                return NULL;
            }
            const char *sname; size_t slen;
            extract_section_name(&name_tok, &sname, &slen);
            basta_map_put(map, sname, slen, container);
        }

        if (!p.had_error && !check(&p, TOK_EOF)) {
            parser_error(&p, "expected '@' section or end of input");
            basta_free(map);
            if (result) *result = p.result;
            return NULL;
        }

        if (result) *result = p.result;
        return map;
    }

    BastaValue *first = parse_value(&p);

    if (p.had_error || check(&p, TOK_EOF)) {
        if (result) *result = p.result;
        return first;
    }

    if (!is_container_start(&p)) {
        parser_error(&p, "expected container (map or array) at top level");
        basta_free(first); if (result) *result = p.result; return NULL;
    }

    BastaValue *arr = basta_value_array();
    if (!arr) { basta_free(first); parser_error(&p, "allocation failed"); if (result) *result = p.result; return NULL; }
    basta_array_push(arr, first);

    while (!p.had_error && !check(&p, TOK_EOF)) {
        if (!is_container_start(&p)) {
            parser_error(&p, "expected container (map or array) at top level");
            basta_free(arr); if (result) *result = p.result; return NULL;
        }
        BastaValue *next = parse_value(&p);
        if (!next || p.had_error) { basta_free(arr); basta_free(next); if (result) *result = p.result; return NULL; }
        basta_array_push(arr, next);
    }

    if (result) *result = p.result;
    return arr;
}

BASTA_API BastaValue *basta_parse_cstr(const char *input, BastaResult *result) {
    return basta_parse(input, strlen(input), result);
}
