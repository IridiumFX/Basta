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
| `atom-labels.cases` | Label / atom grammar: `input → expected structural fingerprint`. 91 cases. |
| `comments.cases` | Comment grammar (`;`, `//`, `/* */`). 16 cases. |
| `strings.cases` | String grammar: raw strings and the multiline quote-run rule. 14 cases. |
| `run_conformance.c` | Reference runner (public Basta API only). Proves the reference implementation matches the corpora, and serves as a worked example for other runners. |
| `superset_check.c` | Executable check of the superset claim — parses the same inputs with *both* libraries and compares. |

The `.cases` files are the source of truth; the runner takes one as its argument.

## Checking the superset claim

`superset_check.c` tests Basta's central claim directly: every document Pasta
accepts must parse under Basta to the same structure, and — since blobs are
binary and cannot appear in text — nothing else should parse under Basta that
Pasta rejects. It needs a Pasta checkout beside this one (the Configlets layout);
it is a verification tool, not a build dependency of Basta.

```bash
gcc -std=c11 -DPASTA_STATIC -DBASTA_STATIC -I../../../Pasta/src/main/h -I../../src/main/h superset_check.c ../../../Pasta/src/main/c/pasta_*.c ../../src/main/c/basta_*.c -lm -o superset_check
./superset_check
```

Expected tail (exit status 0):

```
  OK=27  BROKEN=0  EXT=0
  PASS: identical text language; blob is the only difference.
```

`BROKEN` means the superset property is violated. `EXT` means Basta accepts
something Pasta rejects — permitted in principle for a superset, but for a text
corpus it means the grammars have drifted apart on something other than blobs.
A C-style-comment divergence once sat undetected in exactly that category; this
check exists so it cannot recur silently.

## Corpus format

Plain text. Lines starting with `;` are comments; blank lines are ignored.
Each case is two consecutive content lines:

```
> <input>          the input document (after the "> " prefix)
= <fingerprint>    the expected structural fingerprint, or ERR
```

Within `<input>` **and** `<fingerprint>` alike, `\n` means a newline and `\\` a
literal backslash, so a case can span lines — which line comments and multiline
strings both need. No other escape is recognised.

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
- All three comment forms (`;`, `//`, `/* */`) as `blank`; that delimiters inside
  strings are data, not comments (`"http://x/y"`); and that an unterminated block
  comment or a lone `/` is an error.
- Strings are raw -- no escape sequences, so a backslash is an ordinary
  character -- and the multiline quote-run rule: the body ends at the first run
  of three or more quotes, and extras in that run are content, which is how
  content ending in a quote is written.

Blob values are binary and cannot live in this text corpus. The `blob`
fingerprint kind exists for completeness; blob semantics are exercised directly
by the Basta library suite (`src/test/c/basta_test.c`). Apart from blobs, Basta
and Pasta accept exactly the same language — the case lines here are identical to
those in Pasta's copy of these corpora (only the headers differ, each citing its
own spec), and keeping them so is the check that the superset claim still holds.

## Running the reference runner

From this directory, built straight from the library sources:

```bash
gcc -std=c11 -DBASTA_STATIC -I../../src/main/h -I../../src/main/c run_conformance.c ../../src/main/c/basta_*.c -o run_conformance
```

Then run each corpus:

```bash
./run_conformance atom-labels.cases && ./run_conformance comments.cases && ./run_conformance strings.cases
```

Expected tails:

```
conformance: 91/91 passed
conformance: 16/16 passed
conformance: 14/14 passed
```

Exit status is `0` iff every case matches.

## Checking another implementation

Write a runner in your language that, for each case, parses the input and emits
the fingerprint above (or reports a parse error for `ERR` cases), then diff its
output against the `=` lines. Any deviation is a conformance gap. `run_conformance.c`
is a ~120-line template to copy.
