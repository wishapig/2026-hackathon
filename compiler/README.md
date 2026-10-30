# Offline source compiler

> **非规范文档**：项目唯一需求与验收依据见 [REQUIREMENTS.md](../REQUIREMENTS.md)。本页仅说明实现、使用方式、设计候选或历史验证；其中的约束性表述不能新增、覆盖或缩小规范要求。

Compile the supplied source, without enumerating any particular n/b:

```bash
python3 compiler/compile.py vendor/demo/src/baseline/block_cholesky.cpp \
  --catalog examples/catalog.json -o build/block_cholesky.meta
```

The catalog is a trusted, explicit access/ABI declaration. Its signature and
read/write rectangles are substituted at each source call. Operator names select
contracts, **not dependency formulas**. The runtime derives conflicts from the
resulting regions. Changing the loop bounds changes the compiled program;
changing n/b binds the same artifact. The catalog conservatively describes full
blocks, including `madd`'s read-modify-write output and aliases.

The current implementation is a dependency-free strict structured-C++ parser,
not the planned Clang frontend. It handles the complete supplied demo driver but
is intentionally not a general C++ implementation. This is an explicit current
capability boundary; migrating to Clang can retain the metadata/runtime schema.
Compilation fails if any unsupported statement or unmodeled side effect is
encountered. It does not silently skip unknown function calls or scalar stores.
The only accepted includes are the audited demo declaration header `"kernels.h"`
and `<algorithm>`, `<cmath>`, `<cstddef>`, `<stdexcept>`, `<vector>`, before code.
These are trusted **unmodified** headers; source-local overrides, injected
compiler macros and custom headers are outside the supported input contract.
Unknown includes, macro directives and preprocessor control are rejected rather
than silently discarding transitive macros. Backslash-newline physical line
splicing is rejected before comment lexing, including CRLF and whitespace forms.
Source/catalog must be trusted local inputs; this frontend does not execute a
C++ preprocessor or expand arbitrary headers.

Supported source shape:

- One `int` function with `(const double *A, double *L, int n, int b)` or the
  three-parameter scalar form without `b`, optionally inside namespaces,
  selected with `--entry`. Other definitions/global state are
  rejected. The function ends with `return 0`.
- Structured `for (int v=start; v<stop; v+=step)` and `++v`/`v++`; `<=` is lowered
  to a checked exclusive upper bound. Only forward loops; induction-variable
  dependent bounds/steps and variable shadowing are rejected.
- Immutable `int`/`const int` local initializers, braced integer `if/else`, integer
  arithmetic/comparisons/boolean expressions. Source literals must be decimal
  and fit signed 32-bit int; multi-digit leading-zero/octal literals are rejected
  (rather than misinterpreted as decimal). Known constant source-int overflow or
  division by zero is rejected at compilation. The runtime also enforces source
  signed-int bounds, positive steps, operation budgets and buffer bounds while
  using wider intermediates for overflow detection. The metadata format itself
  permits separately checked signed 64-bit expressions for generic non-source
  programs.
- Calls present in the external catalog, with integer arguments and `&L[expr]`
  pointer arguments. This initial adapter permits calls on the L buffer only;
  A is the input source for the full-matrix copy.
- Exact `std::copy(A, A + n*n, L)` with the demo's `static_cast<std::size_t>(n)`
  variation, lowered to a `copy` semantic node. Arbitrary library calls are not
  accepted as copies.
- Scalar `L[index] = expression` and `+=`, `-=`, `*=`, `/=` stores. Expressions
  support `A[index]`, `L[index]`, finite decimal constants, parentheses, unary
  signs, `+`, `-`, `*`, `/` and `sqrt`/`std::sqrt`. Integer-only constant
  subexpressions preserve checked C++ int arithmetic (`1/2` remains zero).
  Floating operations preserve the expression tree and separate multiply/add
  operations; there is no algebraic reassociation or fused multiply-add lowering.
- Complete strict-upper zeroing loops `row=0..n-1, col=row+1..n-1`, with the exact
  `L[row*n+col]=0` store, lower to `zero_upper`. Other scalar stores retain their
  actual expression and element access, never broadened to full-upper clearing.
- Entry parameter guards that the runtime binder enforces (`b<=0`, `n<=0`,
  `b>n`, `n%b!=0` throwing a literal std exception) may be recorded as metadata
  requirements rather than execution tasks. They must precede computation.
  Other throw paths, early returns, float predicates, local floating variables
  (including `double sum` accumulators), integer mutation, pointer alias
  declarations, macros and arbitrary C++ are rejected. This remains a strict
  subset, not proof that the missing original scalar source is supported.

Source arithmetic must be defined in the original C++ program. The adapter also
checks ABI integer/index bounds before official kernels; the 64-bit interpreter
is not permission to accept undefined signed overflow in a source program.

`examples/wavefront.cpp` and `examples/wavefront_catalog.json` exercise the same
compiler with a different dependency structure and a different operator symbol:

```bash
python3 compiler/compile.py examples/wavefront.cpp \
  --entry wavefront --catalog examples/wavefront_catalog.json \
  -o build/wavefront.meta
```

The scalar example now uses ordinary C++ matrix arithmetic instead of manually
written `scalar_update` calls:

```bash
python3 compiler/compile.py examples/naive_cholesky.cpp \
  --entry naive_cholesky --catalog examples/naive_catalog.json -o build/naive.meta
```

Its empty catalog is sufficient because scalar expression semantics and exact
element access contracts are compiler-defined. A new external operator still
requires a declared catalog and an explicit application ABI adapter.

## Artifact contract

`DAGMETA1` (8 bytes), little-endian uint64 payload byte length, little-endian uint64
FNV-1a payload checksum, then deterministic compact UTF-8 JSON. FNV-1a detects
accidental corruption, not malicious modification or authentic provenance.
SHA-256 source and catalog digests identify the inputs. Optional `--json-debug`
writes readable redundant diagnostics; execution requires only the `.meta` file.

JSON has schema_version=1, source_digest, contract_digest, operators (symbol
strings), and program. Expressions are integer literals, variable-name strings,
or prefix arrays. Statements are let, for, if, call, copy, zero_upper. Every call
contains expanded rectangle reads/writes (`buffer, offset, rows, cols, stride`).
Scalar stores emit `call` nodes with the compiler-reserved symbol
`scalar_expression`. Arguments are a destination L element offset followed by
postfix instructions: 0/load-A plus offset, 1/load-L plus offset, 2/literal plus
four low-to-high uint16 words of an IEEE-754 binary64 value, 3/add, 4/subtract,
5/multiply, 6/divide, 7/sqrt, 8/negate. The expression tree determines instruction
order. Reads enumerate the actual A/L elements, writes contain only the L target;
all regions are 1×1 with stride n. The four literal words avoid narrowing an
encoded 64-bit pattern through the frontend's checked source-int32 evaluator.
This call's application adapter must validate the bytecode and contracts before
execution; it does not substitute for the mandatory blocked public operators.
The frontend marker selects the source signed-int checking policy. Recorded
parameter_guards are evaluated at binding; the runtime also independently
checks mandatory n/b legality and all decoding limits. These fields are not
permission to bypass source guards or integer-domain checks.

The checksum and hashes do not prove catalog correctness: the access contract
must be reviewed against the official implementation and rechecked when the
judge library changes. Metadata construction is offline; its runtime loading,
parameter binding, graph construction and decoding remain in the timed path.
