# Basta conformance corpus — labels & atoms

A portable, implementation-independent test corpus that pins the behaviour of
the **label / atom grammar**: how a bare run of label characters is classified
in value position, and how map keys and section names are read.

Basta is a superset of Pasta, and the label grammar is shared verbatim, so this
corpus is identical to Pasta's. It exists because the grammar was published
before any parser, and independent implementors — following the spec faithfully
— hit the same anomaly (a label spelled like a number, e.g. `123abc`, or a
`0:`/`true:` key). This corpus freezes the agreed behaviour so any
implementation can self-check against the same cases.

## Files

| File | What it is |
|---|---|
| `atom-labels.cases` | The corpus: `input → expected structural fingerprint`. The source of truth. |
| `run_conformance.c` | Reference runner (public Basta API only). Proves the reference implementation matches the corpus, and serves as a worked example for other runners. |

## Corpus format

Plain text. Lines starting with `;` are comments; blank lines are ignored.
Each case is two consecutive content lines:

```
> <input>          the input document (verbatim after the "> " prefix)
= <fingerprint>    the expected structural fingerprint, or ERR
```

The **fingerprint** is a serializer-independent encoding of the parse tree:

```
null | true | false | num | str:TEXT | lbl:TEXT | blob   scalars (by kind)
[f,f,...]                                                array, in order
{k=f,k=f,...}                                            map, document order; k = key text verbatim
ERR                                                      input must be rejected
```

Scalars are encoded by *kind*: every number is `num`, booleans and null by name,
strings and labels with their text, a binary blob as `blob`.

## What it covers

- Maximal munch: digit-led runs that are not valid numbers are single labels
  (`123abc`, `0x1fg`, `1_000`, `12.3.4`).
- Value-position precedence: `keyword > number > label-ref`
  (`true`→bool, `0`→num, `foo`→label).
- Keys and section names are `label`: `{0: 1}`, `{true: false}`, `@0 { … }`.
- `-` is not a labelchar: `{-5: 1}` and `@-5 { … }` are errors (quote to use).
- Regressions that must stay invalid (`{key value}`, `{a: }`, `[1,,2]`).

Blob values are binary and cannot live in this text corpus. The `blob`
fingerprint kind exists for completeness; blob semantics are exercised directly
by the Basta library suite (`src/test/c/basta_test.c`).

## Running the reference runner

From this directory, built straight from the library sources:

```bash
gcc -std=c11 -DBASTA_STATIC -I../../src/main/h -I../../src/main/c \
    run_conformance.c ../../src/main/c/basta_*.c -o run_conformance
./run_conformance atom-labels.cases
```

Expected tail:

```
conformance: 62/62 passed
```

Exit status is `0` iff every case matches.

## Checking another implementation

Write a runner in your language that, for each case, parses the input and emits
the fingerprint above (or reports a parse error for `ERR` cases), then diff its
output against the `=` lines. Any deviation is a conformance gap. `run_conformance.c`
is a ~120-line template to copy.
