# Comparison with luau-native-update.zip

The uploaded update and this fork both resolve Luau inside the MCP process and
reuse `CBMResolvedCall`. Neither requires an external language server. Their
implementation choices differ substantially: the ZIP embeds the original
C++ Luau checker and LanguageServer library behind a C ABI. This fork implements
a smaller set of those behaviors in original C using Tree-sitter and the MCP's
existing scope and arena APIs. Neither is a complete C translation of Luau LSP.

The ZIP was reviewed as third-party source and evidence. Its installer and
embedded instructions were not treated as user instructions or applied to the
fork. All 27 hashes listed in its manifest match the extracted files.

| Area | Previous fork patch | Uploaded update | Changes after comparison |
| --- | --- | --- | --- |
| Architecture | Native C, existing build | C++17 engine, C adapter, CMake and new vendored dependencies | Keep native C as requested |
| Type inference | Lexical callables, tables, aliases, `typeof` receiver aliases | Uses original Luau checker, potentially much broader inference | Preserve bounded C inference; no claim of checker parity |
| References | Invocation records only | Exact function-value reference answers | Emit separate `CALL_REFERENCE` records for proved function values, including imported members, callbacks and returns |
| Shadowing | Native resolver abstained; graph could still guess by name | Recognized unresolved occurrences block textual fallback | Mark exact bound calls and member calls as requiring semantic proof; block unresolved bound/member value references |
| Graph joins | Renamed calls use recognized `lsp_callable_alias` strategy | Emits `luau_native`, absent from the shared invocation/reference alias allowlists | Reuse recognized strategies for both calls and references; exercise renamed members through the real graph pipeline |
| Definition identity | First same-name/start-line definition won | File and full line range, with ambiguity rejection | Require full local line range and unique graph identity; abstain when flattened declarations collide |
| Module values | Exported table members | Checker can resolve returned functions as well | Support `return run` and invocation/reference of its required function value; infer static table initializer members |
| Imports | Assigned literal `require` only | Engine require graph includes dependencies without assignments | Also extract side-effect, returned and nested literal `require` dependencies; reject conflicting reuse of a flat import alias |
| Roblox paths | Conventional Rojo sourcemap and static require paths | Original Roblox platform, configurable sourcemap and definition files | Keep existing C path support; richer script mapping remains a gap |
| Cache and watcher | Export names persist through existing LSP surfaces and compaction | Explicitly incomplete incremental/cache integration; requires retained project snapshots | Keep existing per-file/cross-file pipeline, without a new project coordinator |
| Verification | Initial native suite and two-file graph test executed | Identity/installer/archive helper tests reported; complete engine and MCP never compiled | Add native regressions and real graph tests; independently rerun its 26 identity checks under ASan/UBSan |

The ZIP's own `verification/REPORT.md` clearly labels the engine experimental.
The identity helper passed independently here, but that helper does not verify
the C++ checker adapter, ABI compatibility, Roblox integration or full indexing.
The uploaded C coordinator also passes a standalone strict syntax check against
this pinned MCP checkout with its native build gate enabled. This does not link
or execute the C++ engine.
The missing `luau_native` alias allowlist entry is established by static
comparison with `src/pipeline/lsp_resolve.h`, not a successful engine run.

New tests cover callable-value edges without fabricated invocations, parameter
and member shadowing without name-based graph edges, renamed imported members,
function-returning modules, same-line ambiguous definitions, conflicting
import aliases, unassigned literal dependencies, UTF-8/CRLF byte spans, literal
table keys versus computed value keys, and repeated computed writes.

## Verification in this fork

- `build/c/test-runner luau_lsp`: **18 passed under ASan/UBSan**, without
  sanitizer diagnostics. This includes full graph construction and a genuine
  incremental closure repair after changing only a module's returned callable:
  the unchanged importer was reparsed and its previous CALLS edge replaced.
  The run logged `surface_changed=1`, `dependents=1`, and `route=closure_repair`;
  it did not substitute a full rebuild for this check.
- Grammar, label, import, reference-contract, path and configuration suites:
  **142 passed**. UBSan still reports the pre-existing `qsort(NULL, 0, ...)`
  in unchanged `internal/cbm/doclink_rst.c:1772`.
- Strict native C syntax, changed-line formatting, the no-test-skips check,
  Luau provenance check and `git diff --check`: passed.
- ZIP helper: **26 identity checks passed under ASan/UBSan**; its C coordinator
  passes strict syntax checking. The embedded C++ engine was not built or run.

The MCP test binary used the session's development `-O0` sanitizer build and
`-Wno-error=maybe-uninitialized` for an existing GCC warning in
`src/foundation/mem_core.c`. Default project build flags remain unchanged.
The complete repository test suite and production release build were not run.

## Remaining differences

The C resolver still lacks the original checker's generics, type packs,
structural aliases, metatable and constructor-return inference, and callable
anonymous callback identities. It does not load custom Roblox definition files
or reverse-map `script.Parent` through renamed Rojo ancestors. Conditional
module exports and module re-exports are not inferred. Import extraction is
syntactic; a user-defined function called `require` can still produce a literal
dependency candidate, although native callable resolution does not treat a
lexically bound `require` as the module loader. These are explicit follow-up
areas, not features supplied by the current C implementation.

Luau definitions currently use the extractor's flat graph naming. Where two
lexical declarations share that identity, the resolver abstains rather than
selecting an arbitrary body. Distinguishing them requires coordinated changes
to definition, caller and graph identity generation.
