# Basta User Guide

**BASTA** stands for **Binary And Simple Text Archive**. It is a superset of
[Pasta](https://github.com/IridiumFX/Pasta) that adds one new value type —
the **blob** — for embedding raw binary data directly inside a Pasta document.

Every valid Pasta document is a valid Basta document. The only addition is
the `BASTA_BLOB` value type and its binary wire encoding.

---

## Table of Contents

1. [The Basta Format](#1-the-basta-format)
2. [The C Library](#2-the-c-library)
3. [Reading Basta](#3-reading-basta)
4. [Writing Basta](#4-writing-basta)
5. [Building Values](#5-building-values)
6. [Internals](#6-internals)

---

## 1. The Basta Format

### 1.1 Relation to Pasta

Basta is Pasta with one extra value type. All of Pasta's syntax — maps,
arrays, strings, numbers (including hex `0x` and binary `0b` literals),
booleans, null, label values, named sections, multiline strings,
comments — is unchanged and valid in Basta.

The only new concept is the blob.

### 1.2 The Blob Value

A blob is a raw binary payload embedded in a Basta document. Blobs appear
in **value position**, exactly where any other value would: as a map entry,
as an array element, or as a top-level value.

The canonical pattern is to describe the blob in Pasta text and attach the
payload as a sibling value:

```
{
  type: "image/png",
  width: 1920,
  height: 1080,
  size: 6291456,
  data: <blob>
}
```

The `size` field is optional but recommended: it lets a reader validate or
skip the blob without parsing it, and handles degenerate cases such as a
truncated stream.

### 1.3 Wire Format

A blob is introduced by the **NUL byte (0x00)**. This byte is illegal in
all other positions in a Pasta or Basta document — it is not a valid
character in labels, strings (simple or multiline), numbers, or anywhere
else in the text grammar. The parser therefore dispatches on it without
any ambiguity and without backtracking.

```
0x00                        blob sentinel (1 byte)
<8 bytes big-endian>        blob length N as uint64 (8 bytes)
<N bytes>                   raw binary payload
```

Total wire size: `1 + 8 + N` bytes. After the last payload byte the parser
resumes normal Pasta text.

The payload may contain arbitrary bytes including further `0x00` bytes. A
`0x00` inside a blob payload is not a sentinel: the lexer is consuming a
known-length binary run, not dispatching on character class.

### 1.4 Multiple Blobs

Any number of blobs may appear in a single document. Each blob is
independent: the sentinel and size field are self-contained.

```
{
  vertex_shader:   <blob>,
  fragment_shader: <blob>,
  compute_shader:  <blob>
}
```

### 1.5 Blob in an Array

Blobs are first-class values and may appear as array elements:

```
{
  frames: [<blob>, <blob>, <blob>],
  count: 3
}
```

### 1.6 Blob with Named Sections

Blobs work naturally with Pasta's `@section` syntax:

```
@meta {
  format: "basta",
  producer: "asset-packer"
}

@assets {
  icon:   <blob>,
  shader: <blob>
}
```

### 1.7 Text Portability

A Basta document that contains no blobs is byte-for-byte identical to a
Pasta document and can be parsed by any Pasta implementation. The Basta
library is fully backward-compatible.

---

## 2. The C Library

### 2.1 Building

The library requires C11 and has no external dependencies beyond the C
standard library. It builds a shared library.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Or with presets:

```bash
cmake --preset release
cmake --build --preset release
```

To disable tests:

```bash
cmake -B build -DBASTA_BUILD_TESTS=OFF
```

### 2.2 Linking

**CMake:**

```cmake
find_package(basta REQUIRED)
target_link_libraries(your_target PRIVATE basta)
```

**Manual:**

```bash
gcc -o myapp myapp.c -lbasta
```

### 2.3 Header

```c
#include "basta.h"
```

The header requires `<stdint.h>` (C99+, implied by the C11 requirement).

### 2.4 Project Layout

```
Basta/
  CMakeLists.txt
  CMakePresets.json
  src/
    main/
      h/
        basta.h              Public API
      c/
        basta_internal.h     Internal types (not installed)
        basta_lexer.c        Tokenizer
        basta_parser.c       Recursive descent parser
        basta_value.c        Value tree and query API
        basta_writer.c       Serializer
    test/
      c/
        basta_test.c         Test suite
      resources/
        text_only.basta      Pure-text document (Pasta compatible)
        sections.basta       Named sections, no blobs
  specs/
    Basta.txt                BNF grammar
  docs/
    guide.md                 This file
```

---

## 3. Reading Basta

### 3.1 Parsing

```c
// From a buffer with explicit length (required when document contains blobs)
BastaValue *basta_parse(const char *input, size_t len, BastaResult *result);

// From a null-terminated string (for blob-free, text-only documents)
BastaValue *basta_parse_cstr(const char *input, BastaResult *result);
```

**Always use `basta_parse` with an explicit length for documents that may
contain blobs.** Because the wire format embeds `0x00` bytes, `strlen` will
return a short count for any document containing a blob, causing the parser
to see a truncated input.

**Basic usage:**

```c
// Read a .basta file into a buffer
FILE *fp = fopen("assets.basta", "rb");
fseek(fp, 0, SEEK_END);
size_t len = (size_t)ftell(fp);
rewind(fp);
char *buf = malloc(len);
fread(buf, 1, len, fp);
fclose(fp);

// Parse
BastaResult r;
BastaValue *root = basta_parse(buf, len, &r);
free(buf);

if (!root) {
    fprintf(stderr, "Parse error at %d:%d: %s\n",
            r.line, r.col, r.message);
    return 1;
}

// ... use root ...

basta_free(root);
```

### 3.2 Error Handling

```c
typedef struct {
    BastaError code;
    int        line;
    int        col;
    char       message[256];
} BastaResult;
```

Error codes:

| Code                        | Meaning                                       |
|-----------------------------|-----------------------------------------------|
| `BASTA_OK`                  | Success.                                      |
| `BASTA_ERR_ALLOC`           | Memory allocation failed.                     |
| `BASTA_ERR_SYNTAX`          | General syntax error.                         |
| `BASTA_ERR_UNEXPECTED_TOKEN`| Token not valid in this position.             |
| `BASTA_ERR_UNEXPECTED_EOF`  | Input ended before a complete value.          |
| `BASTA_ERR_BLOB_TRUNCATED`  | Blob sentinel found but size/data incomplete. |

### 3.3 Querying Values

`BastaType` is one of: `BASTA_NULL`, `BASTA_BOOL`, `BASTA_NUMBER`,
`BASTA_STRING`, `BASTA_ARRAY`, `BASTA_MAP`, `BASTA_LABEL`, `BASTA_BLOB`.

All Pasta query functions exist with `basta_` prefix and identical
semantics. The blob-specific accessor is:

```c
const uint8_t *basta_get_blob(const BastaValue *v, size_t *out_len);
```

Returns a pointer into the value tree (valid until `basta_free`) and sets
`*out_len` to the blob byte count. Returns `NULL` and sets `*out_len` to 0
if `v` is not a `BASTA_BLOB`.

### 3.4 Complete Read Example

```c
#include "basta.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Load the file */
    FILE *fp = fopen("sprite.basta", "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    size_t len = (size_t)ftell(fp);
    rewind(fp);
    char *buf = malloc(len);
    fread(buf, 1, len, fp);
    fclose(fp);

    /* Parse */
    BastaResult r;
    BastaValue *root = basta_parse(buf, len, &r);
    free(buf);
    if (!root) {
        fprintf(stderr, "Error: %s at %d:%d\n", r.message, r.line, r.col);
        return 1;
    }

    /* Read the header */
    const char *fmt  = basta_get_string(basta_map_get(root, "format"));
    double      w    = basta_get_number(basta_map_get(root, "width"));
    double      h    = basta_get_number(basta_map_get(root, "height"));
    double      decl = basta_get_number(basta_map_get(root, "size"));
    printf("format=%s  %dx%d  declared_size=%.0f\n", fmt, (int)w, (int)h, decl);

    /* Access the blob */
    size_t blob_len;
    const uint8_t *pixels = basta_get_blob(
        basta_map_get(root, "data"), &blob_len);

    if (pixels) {
        /* Validate against declared size */
        if (blob_len != (size_t)decl)
            fprintf(stderr, "Warning: size mismatch (%zu vs %.0f)\n",
                    blob_len, decl);
        printf("blob: %zu bytes  first_byte=0x%02X\n", blob_len, pixels[0]);
    }

    basta_free(root);
    return 0;
}
```

### 3.5 Memory

Identical to Pasta: call `basta_free` on the root to release the entire
tree. All `const BastaValue*` pointers (including blob data pointers
returned by `basta_get_blob`) become invalid after `basta_free(root)`.
Passing `NULL` to `basta_free` is safe.

---

## 4. Writing Basta

### 4.1 Serializing

```c
char *basta_write(const BastaValue *v, int flags, size_t *out_len);
```

Returns a `malloc`'d buffer. The buffer is always null-terminated past the
end, but **may contain embedded `0x00` bytes** if the tree holds blobs.
`*out_len` (if non-NULL) receives the exact byte count of the document,
excluding the null terminator. Caller must `free()` the buffer.

**Always capture `out_len` when writing documents that may contain blobs.**
Using `strlen` on the returned buffer will give an incorrect result as soon
as the first blob sentinel byte is encountered.

**Flags:**

| Flag              | Value | Effect                                     |
|-------------------|-------|--------------------------------------------|
| `BASTA_PRETTY`    | `0`   | Indented with newlines (default).          |
| `BASTA_COMPACT`   | `1`   | Single-line, minimal whitespace.           |
| `BASTA_SECTIONS`  | `2`   | Emit root map as `@section` containers.    |
| `BASTA_SORTED`    | `4`   | Sort map keys lexicographically.           |

```c
size_t doc_len;
char *doc = basta_write(root, BASTA_PRETTY, &doc_len);

// Write to file (binary mode — fwrite respects doc_len, not null terminator)
FILE *fp = fopen("out.basta", "wb");
fwrite(doc, 1, doc_len, fp);
fclose(fp);
free(doc);
```

### 4.2 Writing to a File

```c
int basta_write_fp(const BastaValue *v, int flags, void *fp);
```

Writes raw bytes directly to a `FILE*` (passed as `void*` to avoid
requiring `<stdio.h>` in the header). The file must be opened in **binary
mode** (`"wb"`) to preserve blob bytes accurately. Returns 0 on success,
-1 on error.

```c
FILE *fp = fopen("assets.basta", "wb");
if (fp) {
    basta_write_fp(root, BASTA_PRETTY | BASTA_SECTIONS, fp);
    fclose(fp);
}
```

### 4.3 Roundtrip Behaviour

A Basta document roundtrips correctly through parse → write → parse.
Blob data is preserved byte-for-byte. Text content behaves identically to
Pasta: comments are stripped, whitespace is normalized, key order is
preserved, strings are verbatim, and hex/binary number formats are retained.

---

## 5. Building Values

### 5.1 Constructors

All Pasta constructors exist with `basta_` prefix, including
`basta_new_number_fmt(double n, int fmt)` for creating hex/binary-tagged
numbers (`BASTA_NUM_HEX`, `BASTA_NUM_BIN`). Query the format with
`basta_get_number_fmt()`. See the Pasta guide for details.

The blob constructor is:

```c
BastaValue *basta_new_blob(const uint8_t *data, size_t len);
```

Copies `len` bytes from `data`. The caller retains ownership of the source
buffer. Pass `NULL` and `0` to create a zero-length blob.

### 5.2 Example

```c
#include "basta.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* Suppose we have pixel data in memory */
    uint8_t pixels[1920 * 1080 * 4]; /* RGBA */
    /* ... fill pixels ... */

    BastaValue *root = basta_new_map();
    basta_set(root, "format", basta_new_string("rgba8"));
    basta_set(root, "width",  basta_new_number(1920));
    basta_set(root, "height", basta_new_number(1080));
    basta_set(root, "size",   basta_new_number(sizeof(pixels)));
    basta_set(root, "data",   basta_new_blob(pixels, sizeof(pixels)));

    size_t doc_len;
    char *doc = basta_write(root, BASTA_PRETTY, &doc_len);

    FILE *fp = fopen("frame.basta", "wb");
    fwrite(doc, 1, doc_len, fp);
    fclose(fp);

    free(doc);
    basta_free(root);
    return 0;
}
```

### 5.3 Ownership

Identical to Pasta: containers take ownership of their children.
Do not free child values individually; free only the root.
Do not insert the same value into multiple containers.

---

## 6. Internals

### 6.1 Architecture

```
  Input buffer (binary-safe)
         |
         v
     +--------+     +--------+     +-----------+
     | Lexer  | --> | Parser | --> | Value Tree|
     +--------+     +--------+     +-----------+
                                        |
                                        v
                                   +--------+
                                   | Writer |
                                   +--------+
                                        |
                                        v
                              Output buffer (binary)
```

The pipeline has four stages, each in its own source file.

### 6.2 Lexer (`basta_lexer.c`)

Identical to Pasta's lexer with one addition: a `TOK_BLOB` token type and
the `lex_blob` function.

**Blob dispatch:** After `skip_blank`, `lexer_next` checks whether the
current byte is `0x00` *before* entering the character switch. Because
`lex_eof` has already been checked, a `0x00` byte at this point is
unambiguously a blob sentinel — not end-of-input.

**`lex_blob`:**
1. Advances past the sentinel byte.
2. Checks that at least 8 bytes remain; returns `TOK_ERROR` with
   `BASTA_ERR_BLOB_TRUNCATED` if not.
3. Decodes the big-endian uint64 length by reading 8 bytes directly from
   the source buffer (no `lex_advance` calls — those track line/column
   numbers and are unsuitable for binary data).
4. Checks that enough data bytes remain.
5. Advances `lex->pos` by the payload length in a single step.
6. Returns a `TOK_BLOB` token whose `start` points at the `0x00` sentinel
   and whose `len` spans the entire wire encoding (`1 + 8 + N` bytes).

**Why a stray `0x00` inside a string scan is not a problem:** The string
scanner (`lex_string`, `lex_mstring`) runs in a separate mode — it loops
consuming bytes until it finds `"` or `"""`. It never invokes the value
dispatch that checks for `0x00`. A zero byte inside a string is simply
scanned past and becomes part of the token payload. The spec explicitly
forbids `0x00` in Pasta text (it is not a `stringchar`), so this path is
reached only when parsing a malformed document; the resulting `BASTA_STRING`
value would contain an embedded NUL, which the application can detect if
desired.

**Token types:**

```
(all Pasta tokens) + TOK_BLOB
```

### 6.3 Parser (`basta_parser.c`)

Identical to Pasta's parser except `parse_value` handles `TOK_BLOB`:

```c
case TOK_BLOB: {
    BastaValue *v = extract_blob(p->current.start, p->current.len);
    if (!v) parser_error(p, "allocation failed");
    advance(p);
    return v;
}
```

`extract_blob` reads the 8-byte big-endian size from the token and copies
the payload bytes into a new `BASTA_BLOB` value. The lexer has already
validated that the data is present, so `extract_blob` cannot fail due to
truncation — only due to allocation failure.

### 6.4 Value Tree (`basta_value.c`)

`BastaValue` gains a `blob` field in its union:

```c
struct BastaValue {
    BastaType type;
    uint8_t   num_fmt;   /* BASTA_NUM_DEC / _HEX / _BIN */
    union {
        int     boolean;
        double  number;
        struct { char    *data; size_t len; } string;   /* STRING, LABEL */
        struct { uint8_t *data; size_t len; } blob;     /* BLOB */
        struct { BastaValue **items; size_t count; size_t cap; } array;
        struct { BastaMember *items;  size_t count; size_t cap; } map;
    } as;
};
```

`basta_free` handles `BASTA_BLOB` by freeing `v->as.blob.data`.

### 6.5 Writer (`basta_writer.c`)

`write_value` gains a `BASTA_BLOB` case:

```c
case BASTA_BLOB: return write_blob(b, v->as.blob.data, v->as.blob.len);
```

`write_blob` emits: sentinel `0x00`, 8 big-endian size bytes, then the
payload bytes via `buf_append`. `buf_append` is binary-safe — it uses
`memcpy` and tracks byte count separately from any null-termination.

The writer's dynamic `Buf` buffer is byte-accurate throughout: it tracks
`len` independently of the null terminator that is maintained at
`data[len]` for C-string convenience. `basta_write` passes this `len`
to the caller via `out_len`.

### 6.6 Platform Considerations

All Pasta platform notes apply. The additional consideration for Basta is
file I/O: **always open Basta files in binary mode** (`"rb"` / `"wb"`) to
prevent platforms (Windows in particular) from translating `\r\n` sequences
or truncating at embedded `0x00` bytes.

The `basta_write_fp` function uses `fwrite` instead of `fputs` precisely
for this reason.
