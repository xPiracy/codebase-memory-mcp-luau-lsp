#include "luau_lsp.h"
#include "scope.h"
#include "../helpers.h"
#include "../lang_specs.h"
#include <string.h>

/* Callable identities and table identities are separate: a table alias keeps
 * its members, while assigning an unknown value clears the binding's proof. */
typedef struct LuauMember {
    const char *table;
    const char *name;
    const char *callee;
    struct LuauMember *next;
} LuauMember;

typedef struct {
    CBMArena *arena;
    const char *source;
    const char *module;
    const char *caller;
    CBMScope *scope;
    LuauMember *members;
    CBMFileResult *local;
    bool cross_pass;
    CBMLSPDef *defs;
    int def_count;
    const char **import_names;
    const char **import_qns;
    int import_count;
    int uncertain_depth;
    CBMResolvedCallArray *out;
} LuauResolver;

static bool luau_is(TSNode node, const char *kind) {
    return !ts_node_is_null(node) && strcmp(ts_node_type(node), kind) == 0;
}

static TSNode luau_field(TSNode node, const char *field) {
    return ts_node_child_by_field_name(node, field, (uint32_t)strlen(field));
}

static const char *luau_text(LuauResolver *ctx, TSNode node) {
    return ts_node_is_null(node) ? NULL : cbm_node_text(ctx->arena, node, ctx->source);
}

static TSNode luau_static_key(TSNode field) {
    TSNode key = luau_field(field, "name");
    if (!luau_is(key, "identifier") || luau_is(ts_node_child(field, 0), "["))
        return (TSNode){0};
    return key;
}

static const char *luau_function_qn(LuauResolver *ctx, TSNode node, const char *name) {
    if (!name)
        return NULL;
    if (ctx->local) {
        uint32_t line = ts_node_start_point(node).row + 1;
        uint32_t end_line = ts_node_end_point(node).row + 1;
        const char *candidate = NULL;
        for (int i = 0; i < ctx->local->defs.count; i++) {
            const CBMDefinition *def = &ctx->local->defs.items[i];
            if (def->start_line == line && def->end_line == end_line &&
                (strcmp(def->label, "Function") == 0 || strcmp(def->label, "Method") == 0) &&
                strcmp(def->name, name) == 0) {
                if (candidate)
                    return NULL;
                candidate = def->qualified_name;
            }
        }
        /* The graph currently flattens Luau nested function names. Different
         * declarations sharing that graph identity cannot be distinguished by
         * a line match, even when the AST scope itself is unambiguous. */
        for (int i = 0; candidate && i < ctx->local->defs.count; i++) {
            const CBMDefinition *def = &ctx->local->defs.items[i];
            if (strcmp(def->qualified_name, candidate) == 0 &&
                (def->start_line != line || def->end_line != end_line))
                return NULL;
        }
        return candidate;
    }
    const char *qn = cbm_arena_sprintf(ctx->arena, "%s.%s", ctx->caller, name);
    for (int i = 0; i < ctx->def_count; i++) {
        if (ctx->defs[i].lang == CBM_LANG_LUAU && strcmp(ctx->defs[i].qualified_name, qn) == 0)
            return ctx->defs[i].qualified_name;
    }
    return NULL;
}

static const CBMType *luau_value_type(LuauResolver *ctx, TSNode node);

static const char *luau_callable(LuauResolver *ctx, TSNode node) {
    if (luau_is(node, "identifier"))
        return cbm_scope_lookup_callable(ctx->scope, luau_text(ctx, node));

    if (luau_is(node, "dot_index_expression") || luau_is(node, "method_index_expression")) {
        const CBMType *table = luau_value_type(ctx, luau_field(node, "table"));
        const char *name = luau_text(
            ctx, luau_field(node, luau_is(node, "method_index_expression") ? "method" : "field"));
        if (!table || table->kind != CBM_TYPE_NAMED || !name)
            return NULL;
        for (LuauMember *m = ctx->members; m; m = m->next) {
            if (strcmp(m->table, table->data.named.qualified_name) == 0 &&
                strcmp(m->name, name) == 0)
                return m->callee;
        }
    }
    return NULL;
}

static const CBMType *luau_value_type(LuauResolver *ctx, TSNode node) {
    if (luau_is(node, "identifier"))
        return cbm_scope_lookup(ctx->scope, luau_text(ctx, node));
    return cbm_type_unknown();
}

static void luau_set_member(LuauResolver *ctx, const char *table, const char *name,
                            const char *callee) {
    if (!table || !name)
        return;
    LuauMember *m = cbm_arena_alloc(ctx->arena, sizeof(*m));
    if (!m)
        return;
    *m = (LuauMember){table, name, callee, ctx->members};
    ctx->members = m;
}

static void luau_clear_members(LuauResolver *ctx, const char *table) {
    for (LuauMember *m = ctx->members; m; m = m->next) {
        if (!m->callee || strcmp(m->table, table) != 0)
            continue;
        LuauMember *newest = ctx->members;
        while (newest && (strcmp(newest->table, table) != 0 || strcmp(newest->name, m->name) != 0))
            newest = newest->next;
        if (newest == m)
            luau_set_member(ctx, table, m->name, NULL);
    }
}

static bool luau_exact_receiver(LuauResolver *ctx, TSNode node) {
    if (luau_is(node, "identifier"))
        return cbm_scope_contains(ctx->scope, luau_text(ctx, node));
    /* A member must be proved through its receiver. Global name lookup cannot
     * establish that an arbitrary receiver owns a same-named function. */
    return luau_is(node, "dot_index_expression") || luau_is(node, "method_index_expression") ||
           luau_is(node, "bracket_index_expression");
}

static void luau_emit_target(LuauResolver *ctx, TSNode site, TSNode spelling_node,
                             const char *callee, CBMResolvedKind kind) {
    if (!callee)
        return;
    const char *spelling = luau_text(ctx, spelling_node);
    const char *target_leaf = callee;
    const char *source_leaf = spelling;
    for (const char *p = callee; *p; p++)
        if (*p == '.' || *p == ':')
            target_leaf = p + 1;
    for (const char *p = spelling; p && *p; p++)
        if (*p == '.' || *p == ':')
            source_leaf = p + 1;
    bool alias = source_leaf && strcmp(source_leaf, target_leaf) != 0;
    uint32_t start = ts_node_start_byte(site), end = ts_node_end_byte(site);
    for (int i = 0; i < ctx->out->count; i++) {
        const CBMResolvedCall *rc = &ctx->out->items[i];
        if (rc->kind == kind && rc->site_start_byte == start && rc->site_end_byte == end &&
            strcmp(rc->caller_qn, ctx->caller) == 0 && strcmp(rc->callee_qn, callee) == 0)
            return;
    }
    cbm_resolvedcall_push(ctx->out, ctx->arena,
                          (CBMResolvedCall){.caller_qn = ctx->caller,
                                            .callee_qn = callee,
                                            .strategy = alias ? "lsp_callable_alias" : "lsp_direct",
                                            .reason = alias ? spelling : NULL,
                                            .confidence = 0.95f,
                                            .kind = kind,
                                            .site_start_byte = start,
                                            .site_end_byte = end});
}

static void luau_emit(LuauResolver *ctx, TSNode node) {
    TSNode name = luau_field(node, "name");
    if (ctx->local && luau_exact_receiver(ctx, name)) {
        for (int i = 0; i < ctx->local->calls.count; i++) {
            CBMCall *call = &ctx->local->calls.items[i];
            if (call->source_origin == CBM_SOURCE_ORIGIN_RAW &&
                call->site_start_byte == ts_node_start_byte(node) &&
                call->site_end_byte == ts_node_end_byte(node) &&
                strcmp(call->enclosing_func_qn, ctx->caller) == 0)
                call->requires_lsp_resolution = true;
        }
    }
    luau_emit_target(ctx, node, name, luau_callable(ctx, name), CBM_RESOLVED_INVOCATION);
}

static void luau_reference(LuauResolver *ctx, TSNode node, bool literal_key) {
    if (!ctx->local)
        return;
    TSNode token = node;
    if (luau_is(node, "dot_index_expression"))
        token = luau_field(node, "field");
    const char *callee = literal_key ? NULL : luau_callable(ctx, node);
    for (int i = 0; i < ctx->local->usages.count; i++) {
        CBMUsage *usage = &ctx->local->usages.items[i];
        if (usage->source_origin != CBM_SOURCE_ORIGIN_RAW ||
            usage->site_start_byte != ts_node_start_byte(token) ||
            usage->site_end_byte != ts_node_end_byte(token) ||
            strcmp(usage->enclosing_func_qn, ctx->caller) != 0)
            continue;
        if (callee) {
            usage->kind = CBM_USAGE_CALL_REFERENCE;
            usage->may_be_call_reference = true;
            luau_emit_target(ctx, token, node, callee, CBM_RESOLVED_CALL_REFERENCE);
        } else if (literal_key || luau_exact_receiver(ctx, node)) {
            usage->semantic_reference_blocked = true;
            usage->semantic_reference_local_shadow = true;
        }
    }
}

static void luau_walk(LuauResolver *ctx, TSNode node, int depth);

static CBMScope *luau_copy_scope(LuauResolver *ctx, CBMScope *scope, int depth) {
    if (!scope || depth > cbm_lsp_max_walk_depth())
        return NULL;
    CBMScope *parent = scope->parent ? luau_copy_scope(ctx, scope->parent, depth + 1) : NULL;
    if (scope->parent && !parent)
        return NULL;
    CBMScope *copy = cbm_scope_push(ctx->arena, parent);
    if (!copy)
        return NULL;
    for (CBMScopeChunk *c = scope->chunks; c; c = c->next)
        for (int i = 0; i < c->used; i++)
            if (!cbm_scope_bind_callable_checked(copy, c->bindings[i].name, c->bindings[i].type,
                                                 c->bindings[i].callable_qn))
                return NULL;
    return copy;
}

static bool luau_has_export(const char *exports, const char *name) {
    size_t len = strlen(name);
    for (const char *p = exports; p && *p;) {
        const char *end = strchr(p, '|');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == len && strncmp(p, name, n) == 0)
            return true;
        p = end ? end + 1 : NULL;
    }
    return false;
}

static const char *luau_import_callable(LuauResolver *ctx, const char *module, const char *name) {
    const char *target = NULL;
    for (int i = 0; i < ctx->def_count; i++) {
        CBMLSPDef *def = &ctx->defs[i];
        if (def->lang != CBM_LANG_LUAU || !def->def_module_qn ||
            strcmp(def->def_module_qn, module) != 0 ||
            !luau_has_export(def->luau_export_names, name))
            continue;
        if (target && strcmp(target, def->qualified_name) != 0)
            return NULL;
        target = def->qualified_name;
    }
    return target;
}

static bool luau_import_alias_unique(LuauResolver *ctx, const char *name) {
    const char *path = NULL;
    if (!ctx->local)
        return true;
    for (int i = 0; i < ctx->local->imports.count; i++) {
        CBMImport *imp = &ctx->local->imports.items[i];
        if (strcmp(imp->local_name, name) != 0)
            continue;
        if (path && strcmp(path, imp->module_path) != 0)
            return false;
        path = imp->module_path;
    }
    return true;
}

static void luau_function_body(LuauResolver *ctx, TSNode node, const char *qn,
                               const CBMType *self_type, int depth) {
    const char *saved_caller = ctx->caller;
    CBMScope *saved_scope = ctx->scope;
    LuauMember *saved_members = ctx->members;
    CBMScope *closure = luau_copy_scope(ctx, saved_scope, 0);
    ctx->scope = closure ? cbm_scope_push(ctx->arena, closure) : NULL;
    if (!ctx->scope) {
        ctx->scope = saved_scope;
        return;
    }
    int saved_uncertain = ctx->uncertain_depth;
    ctx->uncertain_depth = 0;
    ctx->caller = qn;
    if (self_type)
        cbm_scope_bind(ctx->scope, "self", self_type);
    TSNode params = luau_field(node, "parameters");
    for (uint32_t i = 0; i < ts_node_named_child_count(params); i++) {
        TSNode param = ts_node_named_child(params, i);
        if (!luau_is(param, "parameter"))
            continue;
        const char *name = luau_text(ctx, ts_node_named_child(param, 0));
        const CBMType *type = cbm_type_unknown();
        if (ts_node_named_child_count(param) == 2) {
            TSNode annotation = ts_node_named_child(param, 1);
            /* A typeof alias can prove an annotated receiver;
             * unions, generics and unknown aliases deliberately stay unknown. */
            if (luau_is(annotation, "identifier"))
                type = cbm_scope_lookup(saved_scope, cbm_arena_sprintf(ctx->arena, "@type.%s",
                                                                       luau_text(ctx, annotation)));
        }
        if (name)
            cbm_scope_bind(ctx->scope, name, type);
    }
    luau_walk(ctx, luau_field(node, "body"), depth + 1);
    ctx->scope = saved_scope;
    ctx->caller = saved_caller;
    ctx->members = saved_members;
    ctx->uncertain_depth = saved_uncertain;
}

static void luau_assignment(LuauResolver *ctx, TSNode node, bool local, int depth) {
    TSNode vars = ts_node_named_child(node, 0);
    TSNode values = ts_node_named_child(node, 1);
    uint32_t count = ts_node_named_child_count(vars);
    /* Evaluate every RHS before changing any LHS: local a,b = b,a. */
    const char **callees = cbm_arena_alloc(ctx->arena, sizeof(*callees) * (count + 1));
    const CBMType **types = cbm_arena_alloc(ctx->arena, sizeof(*types) * (count + 1));
    if (!callees || !types)
        return;
    for (uint32_t i = 0; i < count; i++) {
        TSNode value = ts_node_named_child(values, i);
        TSNode var = ts_node_named_child(vars, i);
        callees[i] = luau_callable(ctx, value);
        types[i] = luau_value_type(ctx, value);
        if (luau_is(value, "table_constructor")) {
            types[i] =
                cbm_type_named(ctx->arena, cbm_arena_sprintf(ctx->arena, "%s.@%u", ctx->module,
                                                             ts_node_start_byte(value)));
            if (types[i] && types[i]->kind == CBM_TYPE_NAMED) {
                for (uint32_t k = 0; k < ts_node_named_child_count(value); k++) {
                    TSNode field = ts_node_named_child(value, k);
                    TSNode key = luau_static_key(field);
                    if (!ts_node_is_null(key))
                        luau_set_member(ctx, types[i]->data.named.qualified_name,
                                        luau_text(ctx, key),
                                        luau_callable(ctx, luau_field(field, "value")));
                    else if (luau_is(ts_node_child(field, 0), "["))
                        luau_clear_members(ctx, types[i]->data.named.qualified_name);
                }
            }
        }
        if (luau_is(value, "function_definition")) {
            callees[i] = luau_function_qn(ctx, value, luau_text(ctx, var));
            if (callees[i])
                luau_function_body(ctx, value, callees[i], NULL, depth);
        } else {
            luau_walk(ctx, value, depth + 1);
        }
        /* The pipeline supplies already-resolved module QNs. Never guess a
         * filesystem path here or bind every import at file entry. */
        if (luau_is(value, "function_call") && luau_is(luau_field(value, "name"), "identifier") &&
            strcmp(luau_text(ctx, luau_field(value, "name")), "require") == 0 &&
            !cbm_scope_contains(ctx->scope, "require")) {
            const char *name = luau_text(ctx, var);
            for (int j = 0; name && j < ctx->import_count; j++) {
                if (strcmp(ctx->import_names[j], name) == 0 &&
                    luau_import_alias_unique(ctx, name)) {
                    types[i] = cbm_type_named(ctx->arena, ctx->import_qns[j]);
                    callees[i] = luau_import_callable(ctx, ctx->import_qns[j], "@callable");
                    for (int k = 0; k < ctx->def_count; k++) {
                        CBMLSPDef *def = &ctx->defs[k];
                        if (def->lang != CBM_LANG_LUAU || !def->def_module_qn ||
                            strcmp(def->def_module_qn, ctx->import_qns[j]) != 0 ||
                            !def->luau_export_names)
                            continue;
                        const char *p = def->luau_export_names;
                        while (*p) {
                            const char *end = strchr(p, '|');
                            size_t len = end ? (size_t)(end - p) : strlen(p);
                            const char *export_name = cbm_arena_strndup(ctx->arena, p, len);
                            if (export_name && strcmp(export_name, "@callable") != 0)
                                luau_set_member(
                                    ctx, ctx->import_qns[j], export_name,
                                    luau_import_callable(ctx, ctx->import_qns[j], export_name));
                            if (!end)
                                break;
                            p = end + 1;
                        }
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (ctx->uncertain_depth) {
            callees[i] = NULL;
            types[i] = cbm_type_unknown();
        }
        TSNode var = ts_node_named_child(vars, i);
        if (luau_is(var, "identifier")) {
            const char *name = luau_text(ctx, var);
            CBMScope *target = ctx->scope;
            if (!local) {
                while (target->parent)
                    target = target->parent;
                /* Find the actual lexical frame rather than its parent lookup. */
                for (CBMScope *s = ctx->scope; s; s = s->parent) {
                    bool found = false;
                    for (CBMScopeChunk *c = s->chunks; c; c = c->next)
                        for (int j = 0; j < c->used; j++)
                            if (strcmp(c->bindings[j].name, name) == 0)
                                found = true;
                    if (found) {
                        target = s;
                        break;
                    }
                }
            }
            cbm_scope_bind_callable(target, name, types[i], callees[i]);
        } else if (luau_is(var, "bracket_index_expression")) {
            const CBMType *table = luau_value_type(ctx, luau_field(var, "table"));
            if (table && table->kind == CBM_TYPE_NAMED)
                luau_clear_members(ctx, table->data.named.qualified_name);
        } else if (luau_is(var, "dot_index_expression") ||
                   luau_is(var, "method_index_expression")) {
            const CBMType *table = luau_value_type(ctx, luau_field(var, "table"));
            if (table && table->kind == CBM_TYPE_NAMED)
                luau_set_member(ctx, table->data.named.qualified_name,
                                luau_text(ctx, luau_field(var, "field")), callees[i]);
        }
    }
}

static void luau_export(LuauResolver *ctx, const char *name, const char *callee) {
    if (!ctx->local || ctx->cross_pass || !name || !callee)
        return;
    for (int i = 0; i < ctx->local->defs.count; i++) {
        CBMDefinition *def = &ctx->local->defs.items[i];
        if (strcmp(def->qualified_name, callee) == 0) {
            def->luau_export_names =
                def->luau_export_names
                    ? cbm_arena_sprintf(ctx->arena, "%s|%s", def->luau_export_names, name)
                    : cbm_arena_strdup(ctx->arena, name);
        }
    }
}

static void luau_export_return(LuauResolver *ctx, TSNode node) {
    TSNode value = ts_node_named_child(ts_node_named_child(node, 0), 0);
    /* Reserved export marker: '@' cannot begin a Luau property identifier. */
    luau_export(ctx, "@callable", luau_callable(ctx, value));
    if (luau_is(value, "table_constructor")) {
        for (uint32_t i = 0; i < ts_node_named_child_count(value); i++)
            if (luau_is(ts_node_child(ts_node_named_child(value, i), 0), "["))
                return; /* Unknown key may replace any earlier callable export. */
        for (uint32_t i = 0; i < ts_node_named_child_count(value); i++) {
            TSNode field = ts_node_named_child(value, i);
            TSNode key = luau_static_key(field);
            if (!ts_node_is_null(key))
                luau_export(ctx, luau_text(ctx, key),
                            luau_callable(ctx, luau_field(field, "value")));
        }
    } else {
        const CBMType *table = luau_value_type(ctx, value);
        if (table && table->kind == CBM_TYPE_NAMED) {
            for (LuauMember *m = ctx->members; m; m = m->next) {
                if (strcmp(m->table, table->data.named.qualified_name) != 0)
                    continue;
                bool shadowed = false;
                for (LuauMember *newer = ctx->members; newer != m; newer = newer->next)
                    if (strcmp(newer->table, m->table) == 0 && strcmp(newer->name, m->name) == 0)
                        shadowed = true;
                if (!shadowed)
                    luau_export(ctx, m->name, m->callee);
            }
        }
    }
}

/* Conditional writes and loop-carried values need a join. Until a richer
 * flow lattice is available, discard identities for names they may modify. */
static void luau_invalidate_writes(LuauResolver *ctx, TSNode node, int depth) {
    if (ts_node_is_null(node) || depth > cbm_lsp_max_walk_depth() ||
        luau_is(node, "function_declaration") || luau_is(node, "function_definition"))
        return;
    if (luau_is(node, "assignment_statement")) {
        TSNode vars = ts_node_named_child(node, 0);
        for (uint32_t i = 0; i < ts_node_named_child_count(vars); i++) {
            TSNode var = ts_node_named_child(vars, i);
            if (luau_is(var, "identifier")) {
                const char *name = luau_text(ctx, var);
                for (CBMScope *scope = ctx->scope; scope; scope = scope->parent)
                    for (CBMScopeChunk *c = scope->chunks; c; c = c->next)
                        for (int j = 0; j < c->used; j++)
                            if (strcmp(c->bindings[j].name, name) == 0) {
                                c->bindings[j].callable_qn = NULL;
                                c->bindings[j].type = cbm_type_unknown();
                            }
            } else if (luau_is(var, "bracket_index_expression")) {
                const CBMType *table = luau_value_type(ctx, luau_field(var, "table"));
                if (table && table->kind == CBM_TYPE_NAMED)
                    luau_clear_members(ctx, table->data.named.qualified_name);
            } else if (luau_is(var, "dot_index_expression")) {
                const CBMType *table = luau_value_type(ctx, luau_field(var, "table"));
                if (table && table->kind == CBM_TYPE_NAMED)
                    luau_set_member(ctx, table->data.named.qualified_name,
                                    luau_text(ctx, luau_field(var, "field")), NULL);
            }
        }
    }
    for (uint32_t i = 0; i < ts_node_named_child_count(node); i++)
        luau_invalidate_writes(ctx, ts_node_named_child(node, i), depth + 1);
}

static void luau_walk(LuauResolver *ctx, TSNode node, int depth) {
    if (ts_node_is_null(node) || depth > cbm_lsp_max_walk_depth())
        return;
    if (luau_is(node, "identifier")) {
        luau_reference(ctx, node, false);
        return;
    }
    if (luau_is(node, "dot_index_expression")) {
        luau_reference(ctx, node, false);
        luau_walk(ctx, luau_field(node, "table"), depth + 1);
        return;
    }
    if (luau_is(node, "field")) {
        /* Static table keys name properties; computed keys evaluate values. */
        TSNode key = luau_static_key(node);
        if (!ts_node_is_null(key))
            luau_reference(ctx, key, true);
        else
            luau_walk(ctx, luau_field(node, "name"), depth + 1);
        luau_walk(ctx, luau_field(node, "value"), depth + 1);
        return;
    }
    if (luau_is(node, "type_definition")) {
        const char *name = luau_text(ctx, luau_field(node, "name"));
        TSNode value = ts_node_named_child(node, 1);
        const CBMType *type = cbm_type_unknown();
        bool typeof_alias = false;
        for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            if (strcmp(ts_node_type(ts_node_child(node, i)), "typeof") == 0)
                typeof_alias = true;
        if (luau_is(value, "identifier")) {
            const char *key = luau_text(ctx, value);
            type = cbm_scope_lookup(
                ctx->scope, typeof_alias ? key : cbm_arena_sprintf(ctx->arena, "@type.%s", key));
        }
        if (name)
            cbm_scope_bind(ctx->scope, cbm_arena_sprintf(ctx->arena, "@type.%s", name), type);
        return;
    }
    if (luau_is(node, "function_declaration")) {
        TSNode name_node = luau_field(node, "name");
        const char *name = luau_text(ctx, name_node);
        const char *qn = luau_function_qn(ctx, node, name);
        const char *identity = ctx->uncertain_depth ? NULL : qn;
        const CBMType *self = NULL;
        if (luau_is(name_node, "identifier")) {
            cbm_scope_bind_callable(ctx->scope, name, cbm_type_unknown(), identity);
        } else {
            self = luau_value_type(ctx, luau_field(name_node, "table"));
            if (self && self->kind == CBM_TYPE_NAMED)
                luau_set_member(
                    ctx, self->data.named.qualified_name,
                    luau_text(ctx,
                              luau_field(name_node, luau_is(name_node, "method_index_expression")
                                                        ? "method"
                                                        : "field")),
                    identity);
        }
        if (qn)
            luau_function_body(ctx, node, qn,
                               luau_is(name_node, "method_index_expression") ? self : NULL, depth);
        return;
    }
    if (luau_is(node, "function_definition"))
        return; /* Anonymous callback has no graph caller identity yet. */
    if (luau_is(node, "variable_declaration")) {
        luau_assignment(ctx, ts_node_named_child(node, 0), true, depth);
        return;
    }
    if (luau_is(node, "assignment_statement")) {
        luau_assignment(ctx, node, false, depth);
        return;
    }
    if (luau_is(node, "return_statement") && !ctx->uncertain_depth &&
        strcmp(ctx->caller, ctx->module) == 0)
        luau_export_return(ctx, node);
    if (luau_is(node, "function_call")) {
        luau_emit(ctx, node);
        TSNode name = luau_field(node, "name");
        /* Some generic extractors retain a member-callee token as an ordinary
         * usage. It is consumed by this invocation, never a function value. */
        if (luau_is(name, "dot_index_expression") || luau_is(name, "method_index_expression") ||
            luau_is(name, "bracket_index_expression")) {
            if (!luau_is(name, "bracket_index_expression"))
                luau_reference(
                    ctx,
                    luau_field(name, luau_is(name, "method_index_expression") ? "method" : "field"),
                    true);
            luau_walk(ctx, luau_field(name, "table"), depth + 1);
            if (luau_is(name, "bracket_index_expression"))
                luau_walk(ctx, luau_field(name, "field"), depth + 1);
        } else if (!luau_is(name, "identifier")) {
            luau_walk(ctx, name, depth + 1);
        } else {
            luau_reference(ctx, name, true);
        }
        luau_walk(ctx, luau_field(node, "arguments"), depth + 1);
        return;
    }
    CBMScope *saved = ctx->scope;
    bool conditional = luau_is(node, "if_statement") || luau_is(node, "for_statement") ||
                       luau_is(node, "while_statement") || luau_is(node, "repeat_statement");
    if (conditional) {
        luau_invalidate_writes(ctx, node, depth + 1);
        ctx->uncertain_depth++;
    }
    if (luau_is(node, "block") || luau_is(node, "for_statement")) {
        ctx->scope = cbm_scope_push(ctx->arena, saved);
        if (!ctx->scope) {
            ctx->scope = saved;
            if (conditional)
                ctx->uncertain_depth--;
            return;
        }
    }
    if (luau_is(node, "for_statement")) {
        TSNode clause = luau_field(node, "clause");
        TSNode name = luau_field(clause, "name");
        if (!ts_node_is_null(name))
            cbm_scope_bind(ctx->scope, luau_text(ctx, name), cbm_type_unknown());
        else {
            TSNode vars = ts_node_named_child(clause, 0);
            for (uint32_t i = 0; i < ts_node_named_child_count(vars); i++)
                cbm_scope_bind(ctx->scope, luau_text(ctx, ts_node_named_child(vars, i)),
                               cbm_type_unknown());
        }
    }
    for (uint32_t i = 0; i < ts_node_named_child_count(node); i++)
        luau_walk(ctx, ts_node_named_child(node, i), depth + 1);
    ctx->scope = saved;
    if (conditional)
        ctx->uncertain_depth--;
}

void cbm_run_luau_lsp(CBMArena *arena, CBMFileResult *result, const char *source, int source_len,
                      TSNode root) {
    LuauResolver ctx = {.arena = arena,
                        .source = source,
                        .module = result->module_qn,
                        .caller = result->module_qn,
                        .local = result,
                        .out = &result->resolved_calls};
    ctx.scope = cbm_scope_push(arena, NULL);
    if (ctx.scope)
        luau_walk(&ctx, root, 0);
}

void cbm_run_luau_lsp_cross_result(CBMArena *arena, const char *source, int source_len,
                                   const char *module_qn, CBMLSPDef *defs, int def_count,
                                   const char **import_names, const char **import_qns,
                                   int import_count, TSTree *cached_tree, CBMResolvedCallArray *out,
                                   CBMFileResult *result) {
    if (!arena || !source || source_len <= 0 || !module_qn || !out)
        return;
    TSParser *parser = NULL;
    TSTree *tree = cached_tree;
    if (!tree) {
        parser = ts_parser_new();
        if (!parser || !ts_parser_set_language(parser, cbm_ts_language(CBM_LANG_LUAU))) {
            ts_parser_delete(parser);
            return;
        }
        tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)source_len);
    }
    LuauResolver ctx = {.arena = arena,
                        .source = source,
                        .module = module_qn,
                        .caller = module_qn,
                        .local = result,
                        .cross_pass = true,
                        .defs = defs,
                        .def_count = def_count,
                        .import_names = import_names,
                        .import_qns = import_qns,
                        .import_count = import_count,
                        .out = out};
    ctx.scope = cbm_scope_push(arena, NULL);
    if (tree && ctx.scope)
        luau_walk(&ctx, ts_tree_root_node(tree), 0);
    if (!cached_tree)
        ts_tree_delete(tree);
    ts_parser_delete(parser);
}

void cbm_run_luau_lsp_cross(CBMArena *arena, const char *source, int source_len,
                            const char *module_qn, CBMLSPDef *defs, int def_count,
                            const char **import_names, const char **import_qns, int import_count,
                            TSTree *cached_tree, CBMResolvedCallArray *out) {
    cbm_run_luau_lsp_cross_result(arena, source, source_len, module_qn, defs, def_count,
                                  import_names, import_qns, import_count, cached_tree, out, NULL);
}
