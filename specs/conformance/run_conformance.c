/* ====================================================================
 * run_conformance.c — reference runner for the Basta label/atom
 * conformance corpus (atom-labels.cases).
 *
 * It drives the corpus through the PUBLIC Basta API only, so it doubles
 * as a worked example for authors of other implementations: parse each
 * input, encode the result as a structural fingerprint, and compare.
 * The corpus is pure text; a Basta blob would fingerprint as `blob`.
 *
 * Build (against an installed/!built Basta, or straight from source):
 *     gcc -std=c11 -DBASTA_STATIC -I<basta>/src/main/h -I<basta>/src/main/c \
 *         run_conformance.c <basta>/src/main/c/basta_*.c -o run_conformance
 * Run:
 *     ./run_conformance atom-labels.cases
 * Exit status is 0 iff every case matches.
 * ==================================================================== */

#include "basta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- structural fingerprint (serializer-independent) ---- */

typedef struct { char *p; char *end; } Sink;

static void put(Sink *s, const char *str) {
    while (*str && s->p < s->end) *s->p++ = *str++;
}

static void fingerprint(const BastaValue *v, Sink *s) {
    switch (basta_type(v)) {
    case BASTA_NULL:   put(s, "null"); break;
    case BASTA_BOOL:   put(s, basta_get_bool(v) ? "true" : "false"); break;
    case BASTA_NUMBER: put(s, "num"); break;
    case BASTA_STRING: put(s, "str:"); put(s, basta_get_string(v)); break;
    case BASTA_LABEL:  put(s, "lbl:"); put(s, basta_get_label(v));  break;
    case BASTA_BLOB:   put(s, "blob"); break;
    case BASTA_ARRAY:
        put(s, "[");
        for (size_t i = 0; i < basta_count(v); i++) {
            if (i) put(s, ",");
            fingerprint(basta_array_get(v, i), s);
        }
        put(s, "]");
        break;
    case BASTA_MAP:
        put(s, "{");
        for (size_t i = 0; i < basta_count(v); i++) {
            if (i) put(s, ",");
            put(s, basta_map_key(v, i));
            put(s, "=");
            fingerprint(basta_map_value(v, i), s);
        }
        put(s, "}");
        break;
    default: put(s, "?"); break;
    }
}

/* ---- corpus driver ---- */

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
}

/* Corpus inputs are one line, so a case that needs a newline (a line comment
   followed by content, say) writes \n; \\ is a literal backslash.  Rewrites
   in place -- the result is never longer than the input. */
static void unescape(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '\\' && r[1] == 'n')      { *w++ = '\n'; r++; }
        else if (*r == '\\' && r[1] == '\\') { *w++ = '\\'; r++; }
        else                                  { *w++ = *r; }
    }
    *w = '\0';
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "atom-labels.cases";
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open corpus '%s'\n", path); return 2; }

    char line[2048], input[2048], shown[2048];
    int have_input = 0, total = 0, pass = 0, fail = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;

        if (line[0] == '>' && line[1] == ' ') {
            snprintf(input, sizeof(input), "%s", line + 2);
            rstrip(input);
            snprintf(shown, sizeof(shown), "%s", input); /* escaped form, for display */
            unescape(input);
            have_input = 1;
            continue;
        }
        if (line[0] == '=' && line[1] == ' ') {
            char expected[512];
            snprintf(expected, sizeof(expected), "%s", line + 2);
            rstrip(expected);
            unescape(expected);   /* same escapes as the input line */
            if (!have_input) { fprintf(stderr, "corpus error: '=' with no preceding '>'\n"); continue; }
            have_input = 0;
            total++;

            BastaResult r;
            BastaValue *v = basta_parse_cstr(input, &r);
            int ok;
            const char *got;
            char fpbuf[2048];

            if (strcmp(expected, "ERR") == 0) {
                ok  = (v == NULL || r.code != BASTA_OK);
                got = ok ? "ERR" : "(parsed)";
            } else if (v == NULL || r.code != BASTA_OK) {
                ok  = 0;
                got = "ERR";
            } else {
                Sink s = { fpbuf, fpbuf + sizeof(fpbuf) - 1 };
                fingerprint(v, &s);
                *s.p = '\0';
                got = fpbuf;
                ok  = (strcmp(got, expected) == 0);
            }
            if (ok) pass++; else fail++;
            printf("  [%s] %-32s  exp=%-22s got=%s\n",
                   ok ? "PASS" : "FAIL", shown, expected, got);
            basta_free(v);
        }
    }
    fclose(fp);

    printf("\nconformance: %d/%d passed", pass, total);
    if (fail) printf("  (%d FAILED)", fail);
    printf("\n");
    return fail ? 1 : 0;
}
