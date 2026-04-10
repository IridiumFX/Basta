#include "basta_internal.h"
#include <stdio.h>

typedef struct {
    Lexer       lex;
    Token       current;
    BastaResult result;
    int         had_error;
} Parser;

static void parser_init(Parser *p, const char *src, size_t len) {
    lexer_init(&p->lex, src, len);
    p->had_error    = 0;
    p->result.code     = BASTA_OK;
    p->result.line     = 0;
    p->result.col      = 0;
    p->result.sections = 0;
    p->result.message[0] = '\0';
    p->current = lexer_next(&p->lex);
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
    p->current = lexer_next(&p->lex);
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

static int is_key_token(const Parser *p) {
    return check(p, TOK_LABEL) || check(p, TOK_STRING);
}

static BastaValue *parse_value(Parser *p);

/* ------------------------------------------------------------------ */
/*  Literal decoders                                                   */
/* ------------------------------------------------------------------ */

static double parse_number_literal(const char *start, size_t len, uint8_t *out_fmt) {
    *out_fmt = BASTA_NUM_DEC;

    if (len == 3 && memcmp(start, "Inf",  3) == 0)  return INFINITY;
    if (len == 4 && memcmp(start, "-Inf", 4) == 0)  return -INFINITY;
    if (len == 3 && memcmp(start, "NaN",  3) == 0)  return NAN;

    const char *p = start;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    size_t digits_len = len - (size_t)(p - start);

    if (digits_len >= 3 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        *out_fmt = BASTA_NUM_HEX;
        unsigned long long v = 0;
        for (size_t i = 2; i < digits_len; i++) {
            unsigned d;
            if (p[i] >= '0' && p[i] <= '9')      d = (unsigned)(p[i] - '0');
            else if (p[i] >= 'a' && p[i] <= 'f') d = (unsigned)(p[i] - 'a' + 10);
            else                                  d = (unsigned)(p[i] - 'A' + 10);
            v = (v << 4) | d;
        }
        return neg ? -(double)(long long)v : (double)v;
    }

    if (digits_len >= 3 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        *out_fmt = BASTA_NUM_BIN;
        unsigned long long v = 0;
        for (size_t i = 2; i < digits_len; i++)
            v = (v << 1) | (unsigned)(p[i] - '0');
        return neg ? -(double)(long long)v : (double)v;
    }

    char buf[64];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    return strtod(buf, NULL);
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
            uint8_t fmt;
            double n = parse_number_literal(p->current.start, p->current.len, &fmt);
            BastaValue *v = basta_value_number_fmt(n, fmt);
            if (!v) parser_error(p, "allocation failed");
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
            if (!check(&p, TOK_LABEL) && !check(&p, TOK_STRING)) {
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
