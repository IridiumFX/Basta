/* ====================================================================
 * superset_check.c — executable check of Basta's central spec claim:
 *
 *     "Basta is a superset of Pasta.  The grammar is identical to Pasta's
 *      except that the value production gains one additional alternative:
 *      blob."                                    -- specs/Basta.txt
 *
 * The property under test, for every input D:
 *
 *     Pasta accepts D  =>  Basta accepts D, and yields the same structure.
 *
 * Each case is classified:
 *
 *   OK      both agree — both accept with an identical fingerprint, or both
 *           reject.
 *   BROKEN  Pasta accepts but Basta rejects, or both accept and the trees
 *           differ.  Either is a superset violation.
 *   EXT     Pasta rejects, Basta accepts.  This does not break the superset
 *           property (a superset may accept more), but every EXT case is a
 *           document that is NOT portable back to Pasta, so the two grammars
 *           have drifted apart on something other than blobs.  Since blobs
 *           are binary and cannot appear in this text corpus, the expected
 *           result here is EXT=0.
 *
 * A C-style-comment divergence once sat undetected in exactly the EXT
 * category: Basta's lexer accepted // and block comments that neither spec
 * defined and Pasta rejected.  This check exists so that cannot recur silently.
 *
 * Build (needs a Pasta checkout beside the Basta one, as in the Configlets
 * layout — this is a verification tool, not a build dependency of Basta):
 *
 *   gcc -std=c11 -DPASTA_STATIC -DBASTA_STATIC \
 *       -I../../../Pasta/src/main/h -I../../src/main/h \
 *       superset_check.c ../../../Pasta/src/main/c/pasta_*.c \
 *       ../../src/main/c/basta_*.c -lm -o superset_check
 *   ./superset_check
 *
 * Exit status is 0 iff BROKEN == 0 and EXT == 0.
 * ==================================================================== */

#include "pasta.h"
#include "basta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; char *end; } Sink;
static void put(Sink *s, const char *str) {
    while (*str && s->p < s->end) *s->p++ = *str++;
}

static void fp_pasta(const PastaValue *v, Sink *s) {
    switch (pasta_type(v)) {
    case PASTA_NULL:   put(s,"null"); break;
    case PASTA_BOOL:   put(s, pasta_get_bool(v)?"true":"false"); break;
    case PASTA_NUMBER: put(s,"num"); break;
    case PASTA_STRING: put(s,"str:"); put(s,pasta_get_string(v)); break;
    case PASTA_LABEL:  put(s,"lbl:"); put(s,pasta_get_label(v)); break;
    case PASTA_ARRAY:
        put(s,"[");
        for (size_t i=0;i<pasta_count(v);i++){ if(i)put(s,","); fp_pasta(pasta_array_get(v,i),s);}
        put(s,"]"); break;
    case PASTA_MAP:
        put(s,"{");
        for (size_t i=0;i<pasta_count(v);i++){ if(i)put(s,","); put(s,pasta_map_key(v,i)); put(s,"="); fp_pasta(pasta_map_value(v,i),s);}
        put(s,"}"); break;
    default: put(s,"?"); break;
    }
}

static void fp_basta(const BastaValue *v, Sink *s) {
    switch (basta_type(v)) {
    case BASTA_NULL:   put(s,"null"); break;
    case BASTA_BOOL:   put(s, basta_get_bool(v)?"true":"false"); break;
    case BASTA_NUMBER: put(s,"num"); break;
    case BASTA_STRING: put(s,"str:"); put(s,basta_get_string(v)); break;
    case BASTA_LABEL:  put(s,"lbl:"); put(s,basta_get_label(v)); break;
    case BASTA_BLOB:   put(s,"blob"); break;
    case BASTA_ARRAY:
        put(s,"[");
        for (size_t i=0;i<basta_count(v);i++){ if(i)put(s,","); fp_basta(basta_array_get(v,i),s);}
        put(s,"]"); break;
    case BASTA_MAP:
        put(s,"{");
        for (size_t i=0;i<basta_count(v);i++){ if(i)put(s,","); put(s,basta_map_key(v,i)); put(s,"="); fp_basta(basta_map_value(v,i),s);}
        put(s,"}"); break;
    default: put(s,"?"); break;
    }
}

static int n_ok = 0, n_broken = 0, n_ext = 0;

static void check(const char *label, const char *src) {
    PastaResult pr; BastaResult br;
    char pbuf[1024] = "", bbuf[1024] = "";
    PastaValue *pv = pasta_parse(src, strlen(src), &pr);
    BastaValue *bv = basta_parse(src, strlen(src), &br);
    int pok = (pv && pr.code == PASTA_OK);
    int bok = (bv && br.code == BASTA_OK);
    if (pok) { Sink s = {pbuf, pbuf + sizeof pbuf - 1}; fp_pasta(pv, &s); *s.p = 0; }
    if (bok) { Sink s = {bbuf, bbuf + sizeof bbuf - 1}; fp_basta(bv, &s); *s.p = 0; }

    const char *verdict;
    if (pok && !bok)                          { verdict = "BROKEN"; n_broken++; }
    else if (pok && bok && strcmp(pbuf,bbuf)) { verdict = "BROKEN"; n_broken++; }
    else if (!pok && bok)                     { verdict = "EXT";    n_ext++;    }
    else                                       { verdict = "OK";     n_ok++;     }

    printf("  %-7s %-28s P=%-18s B=%s\n", verdict, label,
           pok ? pbuf : "<reject>", bok ? bbuf : "<reject>");
    pasta_free(pv); basta_free(bv);
}

int main(void) {
    printf("=== core grammar ===\n");
    check("empty map",        "{}");
    check("scalars",          "{a:1,b:true,c:null,d:\"s\",e:foo}");
    check("array",            "[1,2,3]");
    check("nested",           "{a:{b:[1,{c:2}]}}");
    check("sections",         "@a { x: 1 } @b [ 2 ]");
    check("numbers",          "[0,-7,3.14,0x1f,0b1010,Inf,-Inf,NaN]");
    check("atom labels",      "{0:1,true:false,123abc:2}");
    check("number strictness","[007,0x,1.,00]");
    check("exponents",        "[1e10,1E5,1e+16,1.5e-5,-2.5e2]");
    check("bad exponents",    "[1e,1ex,007e5]");
    check("hex keeps its e",  "[0x1e,0b1010]");
    check("multiline str",    "{a: \"\"\"line\nline\"\"\"}");
    check("quoted key",       "{\"a b\": 1}");

    printf("\n=== comments (all three forms are `blank`) ===\n");
    check("; preamble",       "; hdr\n{a:1}");
    check("; inline",         "{a:1 ; note\n}");
    check("// line",          "{a:1} // tail");
    check("// preamble",      "// hdr\n{a:1}");
    check("// inside map",    "{a:1, // note\n b:2}");
    check("/* */ block",      "/* hdr */ {a:1}");
    check("/* */ inline",     "{a:1, /* mid */ b:2}");
    check("/* */ multiline",  "{a:1, /* one\ntwo */ b:2}");
    check("mixed styles",     "; s\n// c\n/* b */\n{a:1}");

    printf("\n=== '/' outside comment position ===\n");
    check("/ in string",      "{a: \"http://x/y\"}");
    check("/ in mstring",     "{a: \"\"\"// not a comment\"\"\"}");
    check("bare / value",     "{a: /}");
    check("single / then num","{a: / 1}");
    check("unterminated /*",  "{a:1 /* never closed");

    printf("\n=== summary ===\n");
    printf("  OK=%d  BROKEN=%d  EXT=%d\n", n_ok, n_broken, n_ext);
    if (n_broken) printf("  FAIL: Basta is NOT a strict superset of Pasta.\n");
    else if (n_ext) printf("  FAIL: grammars drifted apart on something other than blobs.\n");
    else            printf("  PASS: identical text language; blob is the only difference.\n");
    return (n_broken || n_ext) ? 1 : 0;
}
