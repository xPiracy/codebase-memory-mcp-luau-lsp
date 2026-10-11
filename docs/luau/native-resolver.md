# Native Luau resolution

Luau uses the vendored Tree-sitter grammar and an original C resolver in
`internal/cbm/lsp/luau_lsp.c`. No language server process, network service,
or C++ Luau type checker is required. Results enter `CBMResolvedCall` and
are consumed by the existing extraction and cross-file graph passes.

The resolver tracks lexical callable identities, parameter shadowing,
reassignment, local table identities, table aliases, dot calls, colon calls,
implicit `self`, and receiver annotations using `typeof` aliases of a known table binding.
Function bodies are analysed in a copy of their closure environment so an
unexecuted function's assignments do not alter its surrounding scope.
Conditional writes and loop-carried values discard uncertain identities.

Module tables returned as `return M` or `return {run = impl}` expose explicit
callable export names. A direct `return run` exposes a callable module value,
encoded by the reserved `@callable` export marker. These names travel through project-wide definitions,
result compaction, and incremental LSP surfaces. Cross-file resolution uses
those exports and the pipeline's resolved import map, rather than registering
private functions as module members. Static table initializer fields carry
callable identities; literal keys are not callable references, and computed
writes discard affected table identities. Conflicting uses of one require
alias are left unresolved because the pipeline's import map is flat.

Proved function values produce separate `CALL_REFERENCE` records for exact
reference-token spans, including callbacks, aliases, returns, and imported
members. Invocations use the entire call span. UTF-8 and CRLF positions remain
byte offsets from the Tree-sitter AST. Bound but unresolved calls and member
calls require native proof, and unresolved bound/member references suppress
textual fallback. Definition matching requires a full local line range and an
unambiguous graph identity; colliding flat declaration names remain unresolved.

Dedicated AST import extraction recognises literal `require` assignments,
side-effect calls, nested calls and returned calls,
`script.Parent` chains, DataModel member chains, static `GetService`, and
static `WaitForChild`. `sourcemap.json` files with a DataModel root register
Rojo ModuleScript virtual paths in the existing package map. Explicit `.lua`
and `.luau` suffixes are stripped when resolving relative dependencies.

This is a bounded initial resolver, not a complete port of the Luau type
checker. Generics, type packs, structural type aliases, metatables, constructor
return inference, dynamic require expressions, and anonymous callback caller
identities are not yet inferred. `script.Parent` paths currently use the source
folder layout; renamed Rojo ancestors require virtual-to-real script mapping.
Only the conventional `sourcemap.json` name is loaded. Unknown expressions
remain unresolved by this native resolver. Unbound bare globals and unsupported
syntax retain the existing graph fallback behavior. A lexically bound value or
a recognized member call cannot acquire a callable edge through that fallback.

Projects using `.lua` for Luau can select the grammar with
`{"extra_extensions": {".lua": "luau"}}` in `.codebase-memory.json`.
The default `.lua` classification remains Lua; `.luau` selects Luau.

## Reference behavior and provenance

The behavioral references are JohnnyMorganz/luau-lsp at commit
`6f90396fa8f6ac253deb3c7e2c11458c2e67227d`:

- `src/operations/GotoDefinition.cpp`: lexical lookup and receiver property lookup.
- `src/operations/CallHierarchy.cpp`: callable identity and exact call occurrences.
- `src/platform/roblox/RobloxSourcemap.cpp`: virtual DataModel paths and real source paths.

The implementation uses fresh C code and the MCP's own scopes, arenas, and
Tree-sitter nodes. It does not copy or translate upstream source line by line.
The upstream handlers depend on Luau's C++ type checker; porting the handlers
alone would not provide equivalent inference. The originality guard now
includes Luau LSP: `bash scripts/check-lsp-originality.sh --lang luau`.

## Validation

`luau_lsp` is registered in the normal C test runner. It covers recursion,
lexical aliases, shadowing, reassignment, table method dispatch, module exports,
private symbols, callable values, returned-function modules, source byte spans,
ambiguous imports and definitions, literal imports, Rojo paths, and parsing without a cached tree.
The grammar regression now requires both fixture functions.

Validation from this implementation session:

- Before the uploaded-update comparison, `build/c/test-runner luau_lsp`: 10 passed,
  including the end-to-end exported-alias CALLS edge and persisted export surface.
- After the comparison, `build/c/test-runner luau_lsp`: **18 passed under
  ASan/UBSan**, including reference edges and absent shadowing edges through a
  real three-file pipeline, and incremental repair of an unchanged importer
  after its module changes the returned callable. See
  [the comparison report](update-comparison.md) for final validation results.
- `build/c/test-runner grammar_regression grammar_labels extraction_imports
  call_reference_contract fqn userconfig`: 142 passed. The grammar tests also cover the
  shared fixture table across languages. UBSan reported an existing
  `qsort(NULL, 0, ...)` call in unchanged `internal/cbm/doclink_rst.c:1772`.
- Luau originality check, changed-line formatting, strict C syntax checks,
  `git diff --check`, and the no-test-skips check passed.
- Full lint could not run because `cppcheck` is not installed.

The test binary used ASan and UBSan with `-O0` for the development build.
`-Wno-error=maybe-uninitialized` was used to keep an existing GCC allocator
warning from aborting that build. The repository's default warning policy
was not changed. The full test suite and production release build have not
been run.
