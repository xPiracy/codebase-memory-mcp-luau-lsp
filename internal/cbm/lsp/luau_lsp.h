#ifndef CBM_LSP_LUAU_LSP_H
#define CBM_LSP_LUAU_LSP_H

#include "go_lsp.h"

/* Native Tree-sitter resolver; no language-server process or Luau C++ runtime. */
void cbm_run_luau_lsp(CBMArena *arena, CBMFileResult *result, const char *source, int source_len,
                      TSNode root);
void cbm_run_luau_lsp_cross(CBMArena *arena, const char *source, int source_len,
                            const char *module_qn, CBMLSPDef *defs, int def_count,
                            const char **import_names, const char **import_qns, int import_count,
                            TSTree *cached_tree, CBMResolvedCallArray *out);

/* Pipeline variant: also joins exact existing call/reference occurrences to
 * suppress textual fallback for lexical shadows and upgrade callable values.
 * Result strings remain owned by its arena; scratch is used only for output. */
void cbm_run_luau_lsp_cross_result(CBMArena *arena, const char *source, int source_len,
                                   const char *module_qn, CBMLSPDef *defs, int def_count,
                                   const char **import_names, const char **import_qns,
                                   int import_count, TSTree *cached_tree, CBMResolvedCallArray *out,
                                   CBMFileResult *result);

#endif
