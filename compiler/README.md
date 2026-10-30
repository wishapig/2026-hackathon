# Offline source compiler

> **非规范文档**：项目唯一需求与验收依据见 [REQUIREMENTS.md](../REQUIREMENTS.md)。本页仅说明实现、使用方式、设计候选或历史验证；其中的约束性表述不能新增、覆盖或缩小规范要求。

The default CMake build analyzes operator implementations with Clang, then parses
algorithm control flow with the Python structured-C++ frontend. These are two
separate steps: using Clang for operator access inference does not make the
algorithm frontend an arbitrary C++ compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target metadata
```

See [operator source analysis](../docs/operator_source_inference.md) for direct
compiler commands and source/include configuration. The generated `.meta` file
contains the runtime-required semantics; `catalog.generated.json` is an optional
analysis report. Explicit `--catalog` remains a compatibility input. Operator
identities select access declarations and ABI bindings, not dependency formulas.

The algorithm parser rejects unsupported statements and unmodeled side effects.
It accepts a strict structured subset, with known includes before the function;
it does not implement arbitrary preprocessing, macros or header expansion.

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
element access contracts are compiler-defined. A new external operator requires inferred or explicitly declared access semantics
and an application ABI adapter.

## Artifact contract

### Default execution-only encoding (R-28)

Default --encoding auto selects the smaller of lean and lean-deflate. No SHA-256,
FNV, source filenames or original local variable names are stored in these formats.

- One byte: 0xA7 (raw binary) or 0xA8 (raw DEFLATE, no zlib wrapper/checksum).
- Unsigned LEB128 decoded payload length for buffer allocation.
- Binary payload: [integer_semantics, operators, buffers, guards, program].
- Literal integers are encoded directly; [0, slot] denotes a lexical variable.
  Slots 0 and 1 bind runtime n and b. Opcode1..15 denotes negation, logical-not,
  +, -, *, /, %, <, <=, >, >=, ==, !=, &&, || respectively.
- let/for declarations use lexical IDs. Operator and buffer identities occur once
  in their respective tables; calls/regions use table IDs. These identities bind
  external implementations and buffers; they are not hidden DAG formulas.
- Only guards already implied by mandatory runtime n/b validation are omitted.
  Loops, branches, short-circuit semantics, parameters and read/write regions stay.

Default compilation does not calculate source/catalog fingerprints. Explicit
--json-debug may put them in an optional offline report, never in the lean file.
Parsing and allocation constraints remain for execution/resource correctness.
Repeated graph/contract audits are default-off; explicit DAG_AUDIT=1 enables the
existing developer audit. Required numerical validation remains unchanged.

### Legacy comparison formats

The following historical formats remain readable and explicitly selectable.
Their hashes/checksums exist only for legacy compatibility, not default output.


Legacy DAGMETA1–6 containers begin with an 8-byte magic, little-endian uint64 decoded
payload byte length, and little-endian uint64 FNV-1a checksum of the decoded payload:

- `DAGMETA1`: the remaining bytes are deterministic compact UTF-8 JSON.
- `DAGMETA2`: the remaining bytes are exactly one zlib stream containing that JSON;
  no external dictionary or algorithm-specific information is needed.
- `DAGMETA3`: the payload contains two raw 32-byte SHA-256 digests (source, catalog),
  followed by compact positional JSON as described below.
- `DAGMETA4`: exactly one zlib stream containing the `DAGMETA3` payload.
- `DAGMETA5`: the two raw input digests followed by the same positional tree in
  the integer/string/array binary grammar below.
- `DAGMETA6`: exactly one zlib stream containing the `DAGMETA5` payload.

Explicit --encoding raw/zlib/compact/compact-zlib/binary/binary-zlib selects a legacy container. CMake DAG_METADATA_ENCODING additionally accepts auto/lean/lean-deflate; auto selects only the two new execution-only representations. The runtime
accepts all six legacy versions plus lean/lean-deflate and requires zlib >= 1.2.9 (Ubuntu package `zlib1g-dev` for
building). Decompression checks the declared output size and memory budget before
allocation, and rejects truncated data, trailing bytes/streams and mismatched
lengths. Loading and decompression stay inside the scored computation interval.

FNV-1a detects
accidental corruption, not malicious modification or authentic provenance.
SHA-256 source and catalog digests identify the inputs. Optional `--json-debug`
writes readable redundant diagnostics; execution requires only the `.meta` file.

Compact positional JSON fixes only schema field positions. Its root is
`[operators, parameter_guards, frontend, program]`; frontend is 0 (generic checked
int64) or 1 (`strict-structured-cpp-v1`). Each node is one of:

```text
[0, name, value]                         # let
[1, var, start, stop, step, body]         # for
[2, condition, then_nodes, else_nodes]   # if
[3, op, args, reads, writes]             # call
[4]                                    # copy
[5]                                    # zero_upper
```

A region is `[buffer, offset, rows, cols, stride]`. Expressions, variable names,
operator names, access regions and input digests are preserved; there is no
algorithm-specific dictionary or implicit task graph. Container versions 3/4
imply schema version 1. The decoder reconstructs the ordinary representation and
runs the same semantic validation as versions 1/2; positional arrays require
exact field counts. A more conservative parser budget accounts for this temporary
reconstruction. The container checksum covers the decoded binary digests and
positional tree together.

Binary versions 5/6 replace only JSON token spelling, using a strict definite-size
subset of CBOR-style headers: the top three bits select nonnegative integer (0),
negative integer `-1-argument` (1), ASCII string (3), or array (4). The low five
bits store arguments 0..23 directly; 24/25/26/27 select 1/2/4/8-byte big-endian
arguments. Strings and arrays use the argument as their length. No other tags,
indefinite lengths or nonminimal argument widths are accepted. Integers must fit
signed int64; strings are limited to 128 ASCII bytes, matching the identifier
contract. The decoder enforces byte, node and depth budgets before allocation.
No atom table, algorithm dictionary or extra program data is built into the
runtime, and all old-format semantic checks still apply after reconstruction.

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
