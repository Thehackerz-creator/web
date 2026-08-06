/**
 * semantic.c — Semantic Analyzer
 * - Walks AST, discovers all variables
 * - Classifies I/O direction (inputs appear in conditions, outputs in actions)
 * - Validates timer values, numeric ranges, type consistency
 * - Detects output variables used as inputs (conflict detection)
 * - Populates the symbol table for code generation
 */

#include "plc_compiler.h"

#define PLC_INT_MIN_VALUE (-32768.0)
#define PLC_INT_MAX_VALUE (32767.0)

static const char *kind_name(SymbolKind kind) {
    switch (kind) {
        case SYM_BOOL: return "BOOL";
        case SYM_INT: return "INT";
        case SYM_REAL: return "REAL";
        case SYM_TIMER: return "TIMER";
        case SYM_STRUCT: return "STRUCT";
        case SYM_FUNCTION_BLOCK: return "FUNCTION_BLOCK";
        case SYM_INSTANCE: return "INSTANCE";
        case SYM_CONST: return "CONST";
        default: return "UNKNOWN";
    }
}

/* ─── Register a variable if not already in symbol table ────────────────── */
static Symbol *register_var(CompilerCtx *ctx, const char *name,
                             IODirection hint, SymbolKind kind, int line) {
    /* Ignore literal values */
    if (strcasecmp(name, "ON")  == 0) return NULL;
    if (strcasecmp(name, "OFF") == 0) return NULL;

    Symbol *s = sym_lookup(ctx, name);
    if (s) {
        s->is_used = 1;
        return s;
    }

    IODirection dir = hint;
    s = sym_insert(ctx, name, dir, kind, line, 0);
    if (s) {
        s->is_used = 1;
        log_info(ctx, "  Discovered variable '%s' → %s (%s) at line %d",
                 name,
                 dir == IO_INPUT  ? "INPUT" :
                 dir == IO_OUTPUT ? "OUTPUT" : "MEMORY",
                 kind == SYM_BOOL ? "BOOL" : kind == SYM_REAL ? "REAL" : "INT",
                 line);
    }
    return s;
}

/* ─── Determine kind from value string ──────────────────────────────────── */
static SymbolKind kind_from_value(const char *val) {
    if (!val || !val[0]) return SYM_BOOL;
    if (strcasecmp(val, "ON") == 0 || strcasecmp(val, "OFF") == 0)
        return SYM_BOOL;
    /* Check if it contains a decimal point → REAL */
    if (strchr(val, '.')) return SYM_REAL;
    /* Pure numeric → INT (could also be BOOL in some contexts) */
    if (isdigit((unsigned char)val[0])) return SYM_INT;
    return SYM_BOOL;
}

static Symbol *lookup_const(CompilerCtx *ctx, const char *name) {
    Symbol *s = sym_lookup(ctx, name);
    return (s && s->kind == SYM_CONST) ? s : NULL;
}

static SymbolKind const_value_kind(const Symbol *s) {
    if (!s || s->kind != SYM_CONST) return SYM_BOOL;
    return kind_from_value(s->const_value);
}

static int literal_is_integral(const char *val) {
    return val && val[0] && strchr(val, '.') == NULL;
}

static int numeric_fits_int(double value) {
    return value >= PLC_INT_MIN_VALUE && value <= PLC_INT_MAX_VALUE;
}

static SymbolKind kind_from_decl_type(const char *type, int *safety_critical, int *safety_estop) {
    if (safety_critical) *safety_critical = 0;
    if (safety_estop) *safety_estop = 0;

    if (!type || !type[0]) return SYM_BOOL;

    if (strcasecmp(type, "SAFE_BOOL") == 0 ||
        strcasecmp(type, "SAFETY_BOOL") == 0 ||
        strcasecmp(type, "CRITICAL_BOOL") == 0) {
        if (safety_critical) *safety_critical = 1;
        return SYM_BOOL;
    }
    if (strcasecmp(type, "SAFE_INT") == 0 ||
        strcasecmp(type, "SAFETY_INT") == 0 ||
        strcasecmp(type, "CRITICAL_INT") == 0) {
        if (safety_critical) *safety_critical = 1;
        return SYM_INT;
    }
    if (strcasecmp(type, "SAFE_REAL") == 0 ||
        strcasecmp(type, "SAFETY_REAL") == 0 ||
        strcasecmp(type, "CRITICAL_REAL") == 0) {
        if (safety_critical) *safety_critical = 1;
        return SYM_REAL;
    }
    if (strcasecmp(type, "ESTOP_BOOL") == 0 ||
        strcasecmp(type, "E_STOP_BOOL") == 0 ||
        strcasecmp(type, "EMERGENCY_STOP_BOOL") == 0) {
        if (safety_estop) *safety_estop = 1;
        return SYM_BOOL;
    }
    if (strcasecmp(type, "REAL") == 0) return SYM_REAL;
    if (strcasecmp(type, "INT") == 0) return SYM_INT;
    if (strcasecmp(type, "TIMER") == 0) return SYM_TIMER;
    return SYM_BOOL;
}

static void apply_decl_metadata(Symbol *s, int safety_critical, int safety_estop,
                                int safety_sil_level) {
    if (!s) return;
    if (safety_critical) s->safety_critical = 1;
    if (safety_estop) s->safety_estop = 1;
    if (safety_sil_level > 0) s->safety_sil_level = safety_sil_level;
}

static void validate_numeric_use(CompilerCtx *ctx, const Symbol *s,
                                 const ASTNode *node, const char *context) {
    if (!ctx || !s || !node || !node->is_numeric) return;

    SymbolKind literal_kind = kind_from_value(node->value);
    if (s->kind == SYM_INT) {
        if (literal_kind == SYM_REAL) {
            log_error(ctx, node->line,
                      "Type mismatch: %s '%s' is INT but literal '%s' is REAL",
                      context, s->name, node->value);
        } else if (!numeric_fits_int(node->numeric_val)) {
            log_error(ctx, node->line,
                      "Integer literal '%s' for %s '%s' is outside IEC INT range %.0f..%.0f",
                      node->value, context, s->name,
                      PLC_INT_MIN_VALUE, PLC_INT_MAX_VALUE);
        }
    } else if (s->kind == SYM_REAL && literal_kind == SYM_INT && literal_is_integral(node->value)) {
        log_warn(ctx,
                 "Type warning at line %d: %s '%s' is REAL but literal '%s' is INT; use a REAL literal such as '%s.0'",
                 node->line, context, s->name, node->value, node->value);
    } else if (s->kind == SYM_BOOL) {
        if (!(node->numeric_val == 0.0 || node->numeric_val == 1.0)) {
            log_error(ctx, node->line,
                      "Type mismatch: %s '%s' is BOOL but literal '%s' is numeric",
                      context, s->name, node->value);
        }
    }
}

/* ─── Walk condition node ─────────────────────────────────────────────────── */
static void analyze_condition(CompilerCtx *ctx, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_COMPARISON: {
            /* Variable on left → must be an input or memory */
            Symbol *rhs_const = (!node->is_numeric) ? lookup_const(ctx, node->value) : NULL;
            SymbolKind k = rhs_const ? const_value_kind(rhs_const) : kind_from_value(node->value);
            Symbol *s = register_var(ctx, node->var_name, IO_INPUT, k, node->line);
            validate_numeric_use(ctx, s, node, "comparison variable");

            /* If it was already registered as OUTPUT, it's a conflict */
            if (s && s->direction == IO_OUTPUT) {
                /* Reclassify as MEMORY to allow read-write */
                s->direction = IO_MEMORY;
                log_warn(ctx, "Variable '%s' used as both input and output — reclassified as MEMORY",
                         node->var_name);
            }

            if (!node->is_numeric &&
                strcasecmp(node->value, "ON") != 0 &&
                strcasecmp(node->value, "OFF") != 0 &&
                !rhs_const) {
                register_var(ctx, node->value, IO_INPUT, SYM_BOOL, node->line);
            }

            break;
        }
        case NODE_AND:
        case NODE_OR:
        case NODE_NOT:
            for (int i = 0; i < node->child_count; i++)
                analyze_condition(ctx, node->children[i]);
            break;
        default: break;
    }
}

/* ─── Walk action / statement linked lists ─────────────────────────────── */
static void analyze_action_node(CompilerCtx *ctx, ASTNode *act) {
    if (!act || act->type != NODE_ACTION || !act->var_name[0]) return;

    SymbolKind k = kind_from_value(act->value);
    Symbol *s = register_var(ctx, act->var_name, IO_OUTPUT, k, act->line);
    validate_numeric_use(ctx, s, act, "assignment target");

    /* Mark conflicts if used as input in same program */
    if (s && s->direction == IO_INPUT) {
        s->direction = IO_MEMORY;
        log_warn(ctx, "Variable '%s' used as both condition input and action output — reclassified as MEMORY",
                 act->var_name);
    }

    /* If assigning a variable (not ON/OFF/number), register that too */
    if (!act->is_numeric &&
        strcasecmp(act->value, "ON")  != 0 &&
        strcasecmp(act->value, "OFF") != 0 &&
        !lookup_const(ctx, act->value)) {
        register_var(ctx, act->value, IO_INPUT, SYM_BOOL, act->line);
    }
}

static void analyze_actions(CompilerCtx *ctx, ASTNode *head) {
    for (ASTNode *act = head; act; act = act->next)
        analyze_action_node(ctx, act);
}

static void analyze_statement_chain(CompilerCtx *ctx, ASTNode *head);

/* ─── Process formal variable declarations ────────────────────────────── */
static void analyze_var_declarations(CompilerCtx *ctx, ASTNode *block) {
    if (!block || block->type != NODE_VAR_DECL) return;

    IODirection dir = IO_MEMORY;
    if (strcmp(block->value, "INPUT") == 0)      dir = IO_INPUT;
    else if (strcmp(block->value, "OUTPUT") == 0) dir = IO_OUTPUT;

    for (int i = 0; i < block->child_count; i++) {
        ASTNode *decl = block->children[i];
        int safety_critical = 0;
        int safety_estop = 0;
        SymbolKind kind = kind_from_decl_type(decl->value, &safety_critical, &safety_estop);

        Symbol *s = sym_insert_scoped(ctx, decl->var_name, dir, kind, ctx->cur_scope, NULL, decl->line, 0);
        if (s) {
            apply_decl_metadata(s,
                                safety_critical || decl->safety_critical,
                                safety_estop || decl->safety_estop,
                                decl->safety_sil_level);
            snprintf(s->unit, MAX_IDENTIFIER_LEN, "%s", decl->unit);
            s->is_used = 1;
            log_info(ctx, "  Declared variable '%s' → %s (%s)%s%s%s",
                     decl->var_name,
                     dir == IO_INPUT ? "INPUT" : (dir == IO_OUTPUT ? "OUTPUT" : "MEMORY"),
                     kind_name(kind),
                     s->safety_critical  ? " [@CRITICAL]"  : "",
                     s->safety_estop     ? " [@ESTOP]"     : "",
                     s->safety_sil_level ? " [@SILx]"      : "");
        }
    }
}

static void analyze_const_decl(CompilerCtx *ctx, ASTNode *node) {
    if (!ctx || !node || node->type != NODE_CONST_DECL || !node->var_name[0])
        return;

    Symbol *existing = sym_lookup(ctx, node->var_name);
    if (existing && existing->kind != SYM_CONST) {
        log_error(ctx, node->line,
                  "Constant '%s' conflicts with an existing symbol",
                  node->var_name);
        return;
    }
    if (existing && existing->kind == SYM_CONST && existing->const_value[0]) {
        log_error(ctx, node->line, "Duplicate constant declaration '%s'", node->var_name);
        return;
    }

    Symbol *s = sym_insert(ctx, node->var_name, IO_MEMORY, SYM_CONST, node->line, 0);
    if (!s) return;

    s->is_used = 1;
    snprintf(s->const_value, MAX_IDENTIFIER_LEN, "%s", node->value);
    snprintf(s->unit, MAX_IDENTIFIER_LEN, "%s", node->unit);
    log_info(ctx, "  Declared constant '%s' = %s%s%s",
             node->var_name, node->value,
             node->unit[0] ? " " : "",
             node->unit[0] ? node->unit : "");
}

static void check_case_duplicates(CompilerCtx *ctx, ASTNode *node) {
    int default_count = 0;
    if (!ctx || !node || node->type != NODE_CASE) return;

    for (int i = 0; i < node->child_count; i++) {
        ASTNode *a = node->children[i];
        if (!a) continue;

        if (strcasecmp(a->value, "DEFAULT") == 0) {
            default_count++;
            if (default_count > 1) {
                log_error(ctx, a->line,
                          "Duplicate DEFAULT branch in CASE '%s'",
                          node->var_name);
            }
            continue;
        }

        for (int j = i + 1; j < node->child_count; j++) {
            ASTNode *b = node->children[j];
            if (!b || strcasecmp(b->value, "DEFAULT") == 0) continue;
            if (strcasecmp(a->value, b->value) == 0) {
                log_error(ctx, b->line,
                          "Duplicate CASE branch value '%s' in CASE '%s'",
                          b->value, node->var_name);
            }
        }
    }
}

/* ─── Recursively analyze AST nodes ─────────────────────────────────────── */
static void analyze_node(CompilerCtx *ctx, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_PROGRAM:
            /* First pass: Process all variable declarations */
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]->type == NODE_VAR_DECL)
                    analyze_var_declarations(ctx, node->children[i]);
                else if (node->children[i]->type == NODE_CONST_DECL)
                    analyze_const_decl(ctx, node->children[i]);
            }
            /* Second pass: Process logic */
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]->type != NODE_VAR_DECL &&
                    node->children[i]->type != NODE_CONST_DECL &&
                    node->children[i]->type != NODE_DOC_COMMENT)
                    analyze_node(ctx, node->children[i]);
            }
            break;

        case NODE_IF: {
            /* child[0] = condition */
            if (node->child_count > 0)
                analyze_condition(ctx, node->children[0]);

            /* Timer validation */
            if (node->has_timer) {
                if (node->timer_value <= 0) {
                    log_error(ctx, node->line, "Timer value must be positive, got %.2f",
                              node->timer_value);
                } else if (node->timer_unit == TIMER_SECONDS && node->timer_value > 3600) {
                    log_warn(ctx, "Timer value %.2f seconds at line %d is very large (>1 hour)",
                             node->timer_value, node->line);
                }
                /* Register synthetic timer variable */
                char timer_name[MAX_IDENTIFIER_LEN];
                snprintf(timer_name, sizeof(timer_name), "_TIMER_%d", ctx->timer_count++);
                Symbol *ts = sym_insert(ctx, timer_name, IO_TIMER_VAR, SYM_TIMER, node->line, 0);
                if (ts) {
                    ts->is_used = 1;
                    /* Bind this synthetic timer back to the IF node for codegen. */
                    node->timer_symbol_index = (int)(ts - ctx->symbols);
                }
            }

            /* child[1] = THEN branch (action list) */
            if (node->child_count > 1 && node->children[1])
                analyze_actions(ctx, node->children[1]->next);

            /* child[2] = ELSE branch (optional) */
            if (node->child_count > 2 && node->children[2])
                analyze_actions(ctx, node->children[2]->next);

            break;
        }

        case NODE_ACTION:
            analyze_actions(ctx, node);
            break;

        case NODE_CONST_DECL:
            analyze_const_decl(ctx, node);
            break;

        case NODE_RECIPE_USE:
            log_info(ctx, "  Recipe/template requested: '%s'", node->var_name);
            break;

        case NODE_WHILE: {
            /* child[0] = condition */
            if (node->child_count > 0)
                analyze_condition(ctx, node->children[0]);
            /* child[1] = body (action list) */
            if (node->child_count > 1 && node->children[1])
                analyze_statement_chain(ctx, node->children[1]->next);
            break;
        }

        case NODE_CASE: {
            /* Register the CASE variable as input */
            register_var(ctx, node->var_name, IO_INPUT, SYM_INT, node->line);
            check_case_duplicates(ctx, node);
            /* Each child is a CASE_BRANCH */
            for (int i = 0; i < node->child_count; i++) {
                ASTNode *branch = node->children[i];
                if (branch && branch->next)
                    analyze_statement_chain(ctx, branch->next);
            }
            break;
        }
case NODE_STRUCT: {
    /* Struct name is a type, not a variable */
    int prev_scope = ctx->cur_scope;
    ctx->cur_scope = ctx->next_scope_id++;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode *decl = node->children[i];
        if (decl->type == NODE_VAR_DECL) {
                int safety_critical = 0;
                int safety_estop = 0;
                SymbolKind kind = kind_from_decl_type(decl->value, &safety_critical, &safety_estop);
                Symbol *s = sym_insert_scoped(ctx, decl->var_name, IO_MEMORY, kind, ctx->cur_scope, node->var_name, decl->line, 0);
                apply_decl_metadata(s,
                                    safety_critical || decl->safety_critical,
                                    safety_estop || decl->safety_estop,
                                    decl->safety_sil_level);
                if (s) snprintf(s->unit, MAX_IDENTIFIER_LEN, "%s", decl->unit);
            }
        }
    ctx->cur_scope = prev_scope;
    break;
}

case NODE_FUNCTION_BLOCK: {
    /* FB name is a type, not a variable */
    int prev_scope = ctx->cur_scope;
    ctx->cur_scope = ctx->next_scope_id++;
    for (int i = 0; i < node->child_count; i++) {
        ASTNode *child = node->children[i];
        if (child->type == NODE_VAR_DECL) {
            /* Variable sections */
            IODirection dir = IO_MEMORY;
            if (strcmp(child->value, "INPUT") == 0) dir = IO_INPUT;
            else if (strcmp(child->value, "OUTPUT") == 0) dir = IO_OUTPUT;
            else dir = IO_VAR_LOCAL;

            for (int j = 0; j < child->child_count; j++) {
                ASTNode *decl = child->children[j];
                int safety_critical = 0;
                int safety_estop = 0;
                SymbolKind kind = kind_from_decl_type(decl->value, &safety_critical, &safety_estop);
                Symbol *s = sym_insert_scoped(ctx, decl->var_name, dir, kind, ctx->cur_scope, node->var_name, decl->line, 0);
                apply_decl_metadata(s,
                                    safety_critical || decl->safety_critical,
                                    safety_estop || decl->safety_estop,
                                    decl->safety_sil_level);
                if (s) snprintf(s->unit, MAX_IDENTIFIER_LEN, "%s", decl->unit);
            }
        } else if (child->type == NODE_CONTRACT) {
            analyze_condition(ctx, child->children[0]);
        } else {
            analyze_node(ctx, child);
        }
    }
    ctx->cur_scope = prev_scope;
    break;
}

        case NODE_STATE_MACHINE: {
            register_var(ctx, node->var_name, IO_MEMORY, SYM_INT, node->line); /* SM state variable */
            for (int i = 0; i < node->child_count; i++) {
                ASTNode *state = node->children[i];
                if (state->type == NODE_STATE) {
                    for (int j = 0; j < state->child_count; j++) {
                        ASTNode *child = state->children[j];
                        if (child->type == NODE_ACTION) analyze_actions(ctx, child->next);
                        else if (child->type == NODE_TRANSITION) analyze_condition(ctx, child->children[0]);
                        else analyze_node(ctx, child);
                    }
                }
            }
            break;
        }

        case NODE_ASSERT: {
            analyze_condition(ctx, node->children[0]);
            break;
        }

        default: break;
    }
}

static void analyze_statement_chain(CompilerCtx *ctx, ASTNode *head) {
    for (ASTNode *stmt = head; stmt; stmt = stmt->next) {
        if (stmt->type == NODE_ACTION)
            analyze_action_node(ctx, stmt);
        else
            analyze_node(ctx, stmt);
    }
}

/* ─── Public entry point ────────────────────────────────────────────────── */
int semantic_analyze(CompilerCtx *ctx) {
    log_info(ctx, "Semantic: starting analysis pass");

    ctx->timer_count = 0;
    analyze_node(ctx, ctx->ast_root);

    /* Assign PLC hardware addresses */
    sym_assign_addresses(ctx);

    if (ctx->sym_count == 0)
        log_warn(ctx, "No variables discovered — DSL may be empty");

    log_info(ctx, "Semantic: found %d variables, %d timers, %d errors, %d warnings",
             ctx->sym_count, ctx->timer_count,
             ctx->error_count, ctx->warning_count);

    return (ctx->error_count == 0) ? 1 : 0;
}
