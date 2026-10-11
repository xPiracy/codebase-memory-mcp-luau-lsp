#include "test_framework.h"
#include "cbm.h"
#include "lsp/luau_lsp.h"
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/lsp_surface.h"
#include "foundation/hash_table.h"
#include "test_helpers.h"
#include "store/store.h"

static CBMFileResult *luau_extract(const char *source) {
    return cbm_extract_file(source, (int)strlen(source), CBM_LANG_LUAU, "test", "main.luau", 0,
                            NULL, NULL);
}

static int luau_invocations(const CBMFileResult *r) {
    int count = 0;
    for (int i = 0; i < r->resolved_calls.count; i++)
        if (r->resolved_calls.items[i].kind == CBM_RESOLVED_INVOCATION)
            count++;
    return count;
}

TEST(luau_lexical_calls) {
    CBMFileResult *r =
        luau_extract("local function foo() foo() end\n"
                     "local f = foo\nf()\nlocal function bar(foo) foo() end\nf = nil\nf()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 2);
    ASSERT_STR_EQ(r->resolved_calls.items[0].callee_qn, "test.main.foo");
    ASSERT_STR_EQ(r->resolved_calls.items[2].caller_qn, "test.main");
    ASSERT_TRUE(r->resolved_calls.items[2].site_end_byte >
                r->resolved_calls.items[2].site_start_byte);
    cbm_free_result(r);
    PASS();
}

TEST(luau_methods_and_table_aliases) {
    CBMFileResult *r = luau_extract("local M = {}\nfunction M.run() end\n"
                                    "function M:go() self:run() end\nlocal alias = M\nalias:go()\n"
                                    "type Receiver = typeof(M)\nlocal function typed(x: Receiver) "
                                    "x:run() end\nM.run = nil\nM.run()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 3);
    cbm_free_result(r);
    PASS();
}

TEST(luau_block_shadow_and_function_side_effects) {
    CBMFileResult *r = luau_extract("local function foo() end\nlocal f = foo\n"
                                    "do local f = nil f() end\nf()\n"
                                    "local function mutate() f = nil end\nf()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 2);
    cbm_free_result(r);
    PASS();
}

TEST(luau_require_ast_paths) {
    CBMFileResult *r = luau_extract(
        "-- require('fake')\nlocal text = \"require('fake')\"\n"
        "local A = require('./a.luau')\nlocal B = require(script.Parent.Parent.Shared)\n"
        "local C = require(game.ReplicatedStorage.Lib)\nlocal D = require(dynamic)\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->imports.count, 3);
    ASSERT_STR_EQ(r->imports.items[0].local_name, "A");
    ASSERT_STR_EQ(r->imports.items[1].module_path, "../Shared");
    ASSERT_STR_EQ(r->imports.items[2].module_path, "@roblox/game/ReplicatedStorage/Lib");
    cbm_free_result(r);
    PASS();
}

TEST(luau_require_side_effects_and_return) {
    CBMFileResult *r = luau_extract("require('./setup')\nreturn require('./module')\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->imports.count, 2);
    ASSERT_STR_EQ(r->imports.items[0].module_path, "./setup");
    ASSERT_STR_EQ(r->imports.items[1].module_path, "./module");
    ASSERT_TRUE(strstr(r->imports.items[0].local_name, "@require.") ==
                r->imports.items[0].local_name);
    cbm_free_result(r);
    PASS();
}

TEST(luau_cross_exports_and_private_functions) {
    const char *source =
        "local U = require('./util')\nlocal function go() U.run() U.private() end\n";
    CBMFileResult *r = luau_extract(source);
    ASSERT_NOT_NULL(r);
    CBMLSPDef defs[] = {
        {.qualified_name = "test.main.go",
         .short_name = "go",
         .label = "Function",
         .def_module_qn = "test.main",
         .lang = CBM_LANG_LUAU},
        {.qualified_name = "test.util.impl",
         .short_name = "impl",
         .label = "Function",
         .def_module_qn = "test.util",
         .lang = CBM_LANG_LUAU,
         .luau_export_names = "run"},
        {.qualified_name = "test.util.private",
         .short_name = "private",
         .label = "Function",
         .def_module_qn = "test.util",
         .lang = CBM_LANG_LUAU},
    };
    const char *names[] = {"U"}, *qns[] = {"test.util"};
    ASSERT_TRUE(cbm_pxc_has_cross_lsp(CBM_LANG_LUAU));
    cbm_pxc_run_one(CBM_LANG_LUAU, r, source, (int)strlen(source), "test.main", defs, 3, names, qns,
                    1);
    ASSERT_EQ(luau_invocations(r), 1);
    ASSERT_STR_EQ(r->resolved_calls.items[0].caller_qn, "test.main.go");
    ASSERT_STR_EQ(r->resolved_calls.items[0].callee_qn, "test.util.impl");
    cbm_free_result(r);
    PASS();
}

TEST(luau_export_surface) {
    CBMFileResult *r = luau_extract("local function impl() end\n"
                                    "return {run = impl, other = impl}\n");
    ASSERT_NOT_NULL(r);
    bool found = false;
    for (int i = 0; i < r->defs.count; i++) {
        if (strcmp(r->defs.items[i].name, "impl") == 0) {
            ASSERT_STR_EQ(r->defs.items[i].luau_export_names, "run|other");
            found = true;
        }
    }
    ASSERT_TRUE(found);
    cbm_free_result(r);
    PASS();
}

TEST(luau_control_flow_and_loop_shadowing) {
    CBMFileResult *r =
        luau_extract("local function foo() end\nlocal f = foo\n"
                     "for f=1,3 do f() end\nf()\nif condition then f = nil end\nf()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 1);
    cbm_free_result(r);
    PASS();
}

TEST(luau_rojo_sourcemap) {
    const char *source =
        "{\"name\":\"Game\",\"className\":\"DataModel\",\"children\":["
        "{\"name\":\"ReplicatedStorage\",\"className\":\"ReplicatedStorage\",\"children\":["
        "{\"name\":\"Util\",\"className\":\"ModuleScript\",\"filePaths\":[\"src/util.luau\"]}]}]}";
    cbm_pkg_entries_t entries;
    cbm_pkg_entries_init(&entries);
    ASSERT_TRUE(cbm_pkgmap_try_parse("sourcemap.json", "sourcemap.json", source,
                                     (int)strlen(source), &entries));
    ASSERT_EQ(entries.count, 1);
    CBMHashTable *map = cbm_pkgmap_build(&entries, 1, "test");
    ASSERT_NOT_NULL(map);
    ASSERT_STR_EQ(cbm_ht_get(map, "@roblox/game/ReplicatedStorage/Util"), "test.src.util");
    cbm_pkgmap_free(map);
    cbm_pkg_entries_free(&entries);
    PASS();
}

TEST(luau_cross_without_cached_tree) {
    const char *source = "local U = require('./util') U.run()";
    CBMLSPDef def = {.qualified_name = "test.util.run",
                     .short_name = "run",
                     .label = "Function",
                     .lang = CBM_LANG_LUAU,
                     .def_module_qn = "test.util",
                     .luau_export_names = "run"};
    CBMArena arena;
    cbm_arena_init(&arena);
    CBMResolvedCallArray out = {0};
    const char *names[] = {"U"}, *qns[] = {"test.util"};
    cbm_run_luau_lsp_cross(&arena, source, (int)strlen(source), "test.main", &def, 1, names, qns, 1,
                           NULL, &out);
    ASSERT_EQ(out.count, 1);
    cbm_arena_destroy(&arena);
    PASS();
}

TEST(luau_callable_values_and_byte_sites) {
    const char *source = "-- café 🦊\r\nlocal function handler() end\r\n"
                         "local alias = handler\r\nlocal function register(fn) end\r\n"
                         "local function go() register(alias) return handler end\r\n";
    CBMFileResult *r = luau_extract(source);
    ASSERT_NOT_NULL(r);
    int references = 0;
    bool argument = false, returned = false;
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        if (rc->kind != CBM_RESOLVED_CALL_REFERENCE)
            continue;
        references++;
        ASSERT_STR_EQ(rc->callee_qn, "test.main.handler");
        if (strcmp(rc->caller_qn, "test.main.go") == 0) {
            if (rc->site_start_byte == (uint32_t)(strstr(source, "alias)") - source)) {
                ASSERT_STR_EQ(rc->strategy, "lsp_callable_alias");
                ASSERT_STR_EQ(rc->reason, "alias");
                ASSERT_EQ(rc->site_end_byte - rc->site_start_byte, strlen("alias"));
                argument = true;
            }
            if (rc->site_start_byte == (uint32_t)(strstr(source, "return handler") - source + 7))
                returned = true;
        }
    }
    ASSERT_EQ(references, 3);
    ASSERT_TRUE(argument);
    ASSERT_TRUE(returned);
    ASSERT_EQ(luau_invocations(r), 1);
    cbm_free_result(r);
    PASS();
}

TEST(luau_table_initializer_and_computed_keys) {
    const char *source = "local function handler() end\n"
                         "local M = {handler = handler}\nM.handler()\n"
                         "local lookup = {[handler] = 1}\nreturn M\n";
    CBMFileResult *r = luau_extract(source);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 1);
    int references = 0;
    uint32_t key = (uint32_t)(strstr(source, "handler = handler") - source);
    for (int i = 0; i < r->resolved_calls.count; i++) {
        const CBMResolvedCall *rc = &r->resolved_calls.items[i];
        ASSERT_NEQ(rc->site_start_byte, key);
        if (rc->kind == CBM_RESOLVED_CALL_REFERENCE)
            references++;
    }
    ASSERT_EQ(references, 2);
    bool exported = false;
    for (int i = 0; i < r->defs.count; i++)
        if (strcmp(r->defs.items[i].name, "handler") == 0) {
            ASSERT_STR_EQ(r->defs.items[i].luau_export_names, "handler");
            exported = true;
        }
    ASSERT_TRUE(exported);
    cbm_free_result(r);
    r = luau_extract("local function handler() end\n"
                     "local M = {run = handler, [unknown] = nil}\nM.run()\n"
                     "M.run = handler\nM[unknown] = nil\nM.run()\n"
                     "return {run = handler, [unknown] = nil}\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 0);
    for (int i = 0; i < r->defs.count; i++)
        ASSERT_NULL(r->defs.items[i].luau_export_names);
    cbm_free_result(r);
    char repeated[4096] = "local function handler() end\nlocal M = {run = handler}\n";
    for (int i = 0; i < 100; i++)
        strcat(repeated, "M[unknown] = nil\n");
    strcat(repeated, "M.run()\n");
    r = luau_extract(repeated);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 0);
    cbm_free_result(r);
    PASS();
}

TEST(luau_shadowed_occurrences_block_fallback) {
    CBMFileResult *r = luau_extract("local function handler() end\n"
                                    "local function go(handler) handler() return handler end\n"
                                    "handler = nil\nhandler()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 0);
    ASSERT_EQ(r->calls.count, 2);
    for (int i = 0; i < r->calls.count; i++)
        ASSERT_TRUE(r->calls.items[i].requires_lsp_resolution);
    bool blocked = false;
    for (int i = 0; i < r->usages.count; i++) {
        const CBMUsage *usage = &r->usages.items[i];
        if (strcmp(usage->enclosing_func_qn, "test.main.go") == 0 &&
            strcmp(usage->ref_name, "handler") == 0) {
            ASSERT_TRUE(usage->semantic_reference_blocked);
            blocked = true;
        }
    }
    ASSERT_TRUE(blocked);
    cbm_free_result(r);
    PASS();
}

TEST(luau_ambiguous_definition_identity) {
    CBMFileResult *r = luau_extract("local function foo() end local function foo() end foo()\n");
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(luau_invocations(r), 0);
    ASSERT_EQ(r->calls.count, 1);
    ASSERT_TRUE(r->calls.items[0].requires_lsp_resolution);
    cbm_free_result(r);
    PASS();
}

TEST(luau_returned_function_import) {
    CBMFileResult *module = luau_extract("local function run() end\nreturn run\n");
    ASSERT_NOT_NULL(module);
    const char *exports = NULL;
    for (int i = 0; i < module->defs.count; i++)
        if (strcmp(module->defs.items[i].name, "run") == 0)
            exports = module->defs.items[i].luau_export_names;
    ASSERT_STR_EQ(exports, "@callable");
    const char *source = "local run = require('./util')\n"
                         "local function go() run() return run end\n";
    CBMFileResult *r = luau_extract(source);
    ASSERT_NOT_NULL(r);
    CBMLSPDef def = {.qualified_name = "test.util.run",
                     .short_name = "run",
                     .label = "Function",
                     .lang = CBM_LANG_LUAU,
                     .def_module_qn = "test.util",
                     .luau_export_names = exports};
    const char *names[] = {"run"}, *qns[] = {"test.util"};
    cbm_pxc_run_one(CBM_LANG_LUAU, r, source, (int)strlen(source), "test.main", &def, 1, names, qns,
                    1);
    ASSERT_EQ(luau_invocations(r), 1);
    ASSERT_EQ(r->resolved_calls.count, 2);
    ASSERT_EQ(r->resolved_calls.items[1].kind, CBM_RESOLVED_CALL_REFERENCE);
    ASSERT_STR_EQ(r->resolved_calls.items[1].callee_qn, "test.util.run");
    cbm_free_result(r);
    cbm_free_result(module);
    PASS();
}

TEST(luau_reused_require_alias_abstains) {
    const char *source = "local U = require('./first')\nU.run()\n"
                         "do local U = require('./second') U.run() end\n";
    CBMFileResult *r = luau_extract(source);
    ASSERT_NOT_NULL(r);
    CBMLSPDef def = {.qualified_name = "test.second.run",
                     .short_name = "run",
                     .label = "Function",
                     .lang = CBM_LANG_LUAU,
                     .def_module_qn = "test.second",
                     .luau_export_names = "run"};
    const char *names[] = {"U"}, *qns[] = {"test.second"};
    cbm_pxc_run_one(CBM_LANG_LUAU, r, source, (int)strlen(source), "test.main", &def, 1, names, qns,
                    1);
    ASSERT_EQ(luau_invocations(r), 0);
    cbm_free_result(r);
    PASS();
}

static int luau_graph_edges(cbm_store_t *store, const char *project, const char *kind,
                            const char *caller, const char *callee) {
    cbm_edge_t *edges = NULL;
    int count = 0, matched = 0;
    if (cbm_store_find_edges_by_type(store, project, kind, &edges, &count) != CBM_STORE_OK)
        return -1;
    for (int i = 0; i < count; i++) {
        cbm_node_t from = {0}, to = {0};
        bool found_from =
            cbm_store_find_node_by_id(store, edges[i].source_id, &from) == CBM_STORE_OK;
        bool found_to = cbm_store_find_node_by_id(store, edges[i].target_id, &to) == CBM_STORE_OK;
        if (found_from && found_to && from.name && to.name && strcmp(from.name, caller) == 0 &&
            strcmp(to.name, callee) == 0)
            matched++;
        cbm_node_free_fields(&from);
        cbm_node_free_fields(&to);
    }
    cbm_store_free_edges(edges, count);
    return matched;
}

TEST(luau_pipeline_references_and_shadowing) {
    const char *tmp = th_mktempdir("cbm_luau_ref");
    ASSERT_NOT_NULL(tmp);
    char main_path[512], util_path[512], fn_path[512], db_path[512];
    snprintf(main_path, sizeof(main_path), "%s/main.luau", tmp);
    snprintf(util_path, sizeof(util_path), "%s/util.luau", tmp);
    snprintf(fn_path, sizeof(fn_path), "%s/fn.luau", tmp);
    snprintf(db_path, sizeof(db_path), "%s/graph.db", tmp);
    ASSERT_EQ(th_write_file(main_path,
                            "-- café 🦊\r\nlocal U = require('./util')\r\n"
                            "local run = require('./fn')\r\nlocal function handler() end\r\n"
                            "local function accept(fn) end\r\n"
                            "local function go() accept(U.run) return U.run end\r\n"
                            "local function invoke() run() end\r\n"
                            "local function blocked(handler) handler() return handler end\r\n"
                            "local function member(obj) obj.handler() return obj.handler end\r\n"
                            "local function keyonly() end\r\n"
                            "local function fields() return {keyonly = nil} end\r\n"),
              0);
    ASSERT_EQ(th_write_file(util_path, "local function impl() end\nreturn {run = impl}\n"), 0);
    ASSERT_EQ(th_write_file(fn_path, "local function execute() end\n"
                                     "local function replacement() end\nreturn execute\n"),
              0);
    cbm_pipeline_t *pipeline = cbm_pipeline_new(tmp, db_path, CBM_MODE_FULL);
    ASSERT_NOT_NULL(pipeline);
    ASSERT_EQ(cbm_pipeline_run(pipeline), 0);
    char project[256];
    snprintf(project, sizeof(project), "%s", cbm_pipeline_project_name(pipeline));
    cbm_pipeline_free(pipeline);
    cbm_store_t *store = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(luau_graph_edges(store, project, "CALL_REFERENCE", "go", "impl"), 1);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "go", "impl"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "go", "accept"), 1);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "invoke", "execute"), 1);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "blocked", "handler"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "USAGE", "blocked", "handler"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "CALL_REFERENCE", "blocked", "handler"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "member", "handler"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "USAGE", "member", "handler"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "CALL_REFERENCE", "member", "handler"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "CALL_REFERENCE", "fields", "keyonly"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "USAGE", "fields", "keyonly"), 0);
    cbm_store_close(store);
    /* Changing only an exported callable must refresh the unchanged importer
     * from the persisted Luau surface instead of retaining its previous edge. */
    ASSERT_EQ(th_write_file(fn_path, "local function execute() end\n"
                                     "local function replacement() end\nreturn replacement\n"),
              0);
    pipeline = cbm_pipeline_new(tmp, db_path, CBM_MODE_FULL);
    ASSERT_NOT_NULL(pipeline);
    ASSERT_EQ(cbm_pipeline_run(pipeline), 0);
    cbm_pipeline_free(pipeline);
    store = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "invoke", "execute"), 0);
    ASSERT_EQ(luau_graph_edges(store, project, "CALLS", "invoke", "replacement"), 1);
    ASSERT_EQ(luau_graph_edges(store, project, "CALL_REFERENCE", "go", "impl"), 1);
    cbm_store_close(store);
    th_cleanup(tmp);
    PASS();
}

TEST(luau_pipeline_exported_alias_edge) {
    const char *tmp = th_mktempdir("cbm_luau");
    ASSERT_NOT_NULL(tmp);
    char main_path[512], util_path[512], db_path[512];
    snprintf(main_path, sizeof(main_path), "%s/main.luau", tmp);
    snprintf(util_path, sizeof(util_path), "%s/util.luau", tmp);
    snprintf(db_path, sizeof(db_path), "%s/graph.db", tmp);
    ASSERT_EQ(
        th_write_file(main_path,
                      "local U = require('./util.luau')\nlocal function go() U.run() end\ngo()\n"),
        0);
    ASSERT_EQ(th_write_file(util_path, "local function impl() end\nreturn {run = impl}\n"), 0);
    cbm_pipeline_t *pipeline = cbm_pipeline_new(tmp, db_path, CBM_MODE_FULL);
    ASSERT_NOT_NULL(pipeline);
    ASSERT_EQ(cbm_pipeline_run(pipeline), 0);
    char project[256];
    snprintf(project, sizeof(project), "%s", cbm_pipeline_project_name(pipeline));
    cbm_pipeline_free(pipeline);
    cbm_store_t *store = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(store);
    cbm_node_t *callers = NULL, *targets = NULL;
    int caller_count = 0, target_count = 0;
    cbm_store_find_nodes_by_name(store, project, "go", &callers, &caller_count);
    cbm_store_find_nodes_by_name(store, project, "impl", &targets, &target_count);
    bool found = false;
    for (int i = 0; i < caller_count; i++) {
        cbm_edge_t *edges = NULL;
        int edge_count = 0;
        cbm_store_find_edges_by_source_type(store, callers[i].id, "CALLS", &edges, &edge_count);
        for (int j = 0; j < edge_count; j++)
            for (int k = 0; k < target_count; k++)
                if (edges[j].target_id == targets[k].id && edges[j].properties_json &&
                    strstr(edges[j].properties_json, "lsp_callable_alias"))
                    found = true;
        cbm_store_free_edges(edges, edge_count);
    }
    cbm_store_free_nodes(callers, caller_count);
    cbm_store_free_nodes(targets, target_count);
    cbm_lsp_surface_row_t *rows = NULL;
    int row_count = 0;
    cbm_store_get_lsp_surfaces(store, project, &rows, &row_count);
    bool exported_surface = false;
    for (int i = 0; i < row_count; i++)
        if (rows[i].defs_json && strstr(rows[i].defs_json, "\"lex\":\"run\""))
            exported_surface = true;
    cbm_store_free_lsp_surfaces(rows, row_count);
    cbm_store_close(store);
    th_cleanup(tmp);
    ASSERT_TRUE(found);
    ASSERT_TRUE(exported_surface);
    PASS();
}

SUITE(luau_lsp) {
    RUN_TEST(luau_lexical_calls);
    RUN_TEST(luau_methods_and_table_aliases);
    RUN_TEST(luau_block_shadow_and_function_side_effects);
    RUN_TEST(luau_require_ast_paths);
    RUN_TEST(luau_require_side_effects_and_return);
    RUN_TEST(luau_cross_exports_and_private_functions);
    RUN_TEST(luau_export_surface);
    RUN_TEST(luau_control_flow_and_loop_shadowing);
    RUN_TEST(luau_rojo_sourcemap);
    RUN_TEST(luau_cross_without_cached_tree);
    RUN_TEST(luau_callable_values_and_byte_sites);
    RUN_TEST(luau_table_initializer_and_computed_keys);
    RUN_TEST(luau_shadowed_occurrences_block_fallback);
    RUN_TEST(luau_ambiguous_definition_identity);
    RUN_TEST(luau_returned_function_import);
    RUN_TEST(luau_reused_require_alias_abstains);
    RUN_TEST(luau_pipeline_references_and_shadowing);
    RUN_TEST(luau_pipeline_exported_alias_edge);
}
