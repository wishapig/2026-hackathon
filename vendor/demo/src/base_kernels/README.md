# Base Kernels

The following are the source code of the base kernels made public for contestants to read and debug locally:

- `kernels_impl.cpp`: actual computation implementation
- `kernels_public.cpp`: public exported symbol wrappers
- `kernels_internal.h`: internal implementation declarations

Notes:

- These source files can be made publicly available to help contestants understand the operator semantics, data layout, and baseline behavior.
- Contestants may conduct experiments or debugging locally based on these source files.
- However, during official evaluation, the evaluators will forcibly link the official version of the contest's judge operator library.
- Contestant submissions must not redefine, replace, or bypass the official `cholesky`, `trsm`, or `madd`.