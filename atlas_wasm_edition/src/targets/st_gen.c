/**
 * st_gen.c — Shared Structured Text Generation Logic
 *
 * Centralizes AST-to-ST conversion so that codegen.c and export_plcopen.c
 * share the same high-quality output logic.
 */

#include "plc_compiler.h"

static const char *cmp_to_st(CmpOp op) {
    switch (op) {
        case CMP_EQ:  return "=";
        case CMP_NEQ: return "<>";
        case CMP_GTE: return ">=";
        case CMP_LTE: return "<=";
        case CMP_GT:  return ">";
        case CMP_LT:  return "<";
        default:      return "=";
    }
}

static const char *val_to_st(CompilerCtx *ctx, const char *val) {
    if (strcasecmp(val, "ON")  == 0) return "TRUE";
    if (strcasecmp(val, "OFF") == 0) return "FALSE";
    Symbol *s = sym_lookup(ctx, val);
    if (s) return s->st_name;
    return val;
}

static const char *resolve_st(CompilerCtx *ctx, const char *name) {
    if (strcasecmp(name, "ON")  == 0) return "TRUE";
    if (strcasecmp(name, "OFF") == 0) return "FALSE";
    Symbol *s = sym_lookup(ctx, name);
    return s ? s->st_name : name;
}

static int bounded_len(const char *s, int max) {
    int len = 0;
    if (!s || max <= 0) return 0;
    while (len < max && s[len]) len++;
    return len;
}

static void st_append(char *out, int max, const char *fmt, ...) {
    if (!out || max <= 0) return;

    int len = bounded_len(out, max);
    if (len >= max - 1) {
        out[max - 1] = '\0';
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(out + len, (size_t)(max - len), fmt, args);
    va_end(args);
    out[max - 1] = '\0';
}

static void timer_literal(ASTNode *node, char *out, int outlen) {
    double ms = 0.0;
    if (!node || !out || outlen <= 0) return;

    switch (node->timer_unit) {
        case TIMER_SECONDS:      ms = node->timer_value * 1000.0; break;
        case TIMER_MINUTES:      ms = node->timer_value * 60000.0; break;
        case TIMER_MILLISECONDS: ms = node->timer_value; break;
    }
    snprintf(out, (size_t)outlen, "T#%.0fms", ms);
    out[outlen - 1] = '\0';
}

static const char *timer_name_for(CompilerCtx *ctx, ASTNode *node) {
    if (node && node->timer_symbol_index >= 0 && node->timer_symbol_index < ctx->sym_count)
        return ctx->symbols[node->timer_symbol_index].st_name;
    return "_TIMER_0";
}

void st_gen_condition(CompilerCtx *ctx, char *out, int max, ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_COMPARISON: {
            const char *var = resolve_st(ctx, node->var_name);
            const char *op  = cmp_to_st(node->cmp_op);
            const char *val = node->is_numeric ? node->value : val_to_st(ctx, node->value);
            st_append(out, max, "%s %s %s", var, op, val);
            break;
        }
        case NODE_AND:
            st_append(out, max, "(");
            st_gen_condition(ctx, out, max, node->children[0]);
            st_append(out, max, " AND ");
            st_gen_condition(ctx, out, max, node->children[1]);
            st_append(out, max, ")");
            break;
        case NODE_OR:
            st_append(out, max, "(");
            st_gen_condition(ctx, out, max, node->children[0]);
            st_append(out, max, " OR ");
            st_gen_condition(ctx, out, max, node->children[1]);
            st_append(out, max, ")");
            break;
        case NODE_NOT:
            st_append(out, max, "NOT(");
            st_gen_condition(ctx, out, max, node->children[0]);
            st_append(out, max, ")");
            break;
        default: break;
    }
}

static void st_gen_single_action(CompilerCtx *ctx, char *out, int max, ASTNode *act, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    if (!act || act->type != NODE_ACTION || !act->var_name[0]) return;

    const char *var = resolve_st(ctx, act->var_name);
    const char *val = act->is_numeric ? act->value : val_to_st(ctx, act->value);
    st_append(out, max, "%s%s := %s;\n", pad, var, val);
}

void st_gen_actions(CompilerCtx *ctx, char *out, int max, ASTNode *head, int indent) {
    for (ASTNode *act = head; act; act = act->next) {
        if (act->type != NODE_ACTION || !act->var_name[0]) continue;
        /* Special case: entry/exit are wrappers */
        if (strcmp(act->var_name, "ENTRY") == 0 || strcmp(act->var_name, "EXIT") == 0) {
            st_gen_actions(ctx, out, max, act->next, indent);
            continue;
        }
        st_gen_single_action(ctx, out, max, act, indent);
    }
}

void st_gen_statement(CompilerCtx *ctx, char *out, int max, ASTNode *node, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    if (!node) return;

    if (node->type == NODE_IF) {
        char cond[512] = {0};
        const char *timer_st = NULL;
        st_gen_condition(ctx, cond, sizeof(cond), node->children[0]);
        
        if (node->has_timer) {
            char lit[32];
            timer_literal(node, lit, sizeof(lit));
            timer_st = timer_name_for(ctx, node);

            if (ctx->target == PLC_ROCKWELL_AOI) {
                st_append(out, max, "%sTON(%s, %s, %s);\n", pad, timer_st, cond, lit);
                st_append(out, max, "%sIF %s.DN THEN\n", pad, timer_st);
            } else {
                st_append(out, max, "%s%s(IN := %s, PT := %s);\n", pad, timer_st, cond, lit);
                st_append(out, max, "%sIF %s.Q THEN\n", pad, timer_st);
            }
        } else {
            st_append(out, max, "%sIF %s THEN\n", pad, cond);
        }

        if (node->child_count > 1 && node->children[1])
            st_gen_actions(ctx, out, max, node->children[1]->next, indent + 4);
        
        if (node->child_count > 2 && node->children[2] && node->children[2]->next) {
            st_append(out, max, "%sELSE\n", pad);
            st_gen_actions(ctx, out, max, node->children[2]->next, indent + 4);
        }
        st_append(out, max, "%sEND_IF;\n", pad);

    } else if (node->type == NODE_WHILE) {
        char cond[512] = {0};
        if (node->child_count < 2) return;
        st_gen_condition(ctx, cond, sizeof(cond), node->children[0]);
        st_append(out, max, "%sWHILE %s DO\n", pad, cond);
        for (ASTNode *stmt = node->children[1]->next; stmt; stmt = stmt->next)
            st_gen_statement(ctx, out, max, stmt, indent + 4);
        st_append(out, max, "%sEND_WHILE;\n", pad);

    } else if (node->type == NODE_CASE) {
        const char *var = resolve_st(ctx, node->var_name);
        st_append(out, max, "%sCASE %s OF\n", pad, var);

        for (int i = 0; i < node->child_count; i++) {
            ASTNode *branch = node->children[i];
            char branch_pad[64] = {0};
            for (int j = 0; j < indent + 4 && j < 60; j++) branch_pad[j] = ' ';

            if (!branch) continue;
            if (strcasecmp(branch->value, "DEFAULT") == 0)
                st_append(out, max, "%sELSE\n", branch_pad);
            else
                st_append(out, max, "%s%s:\n", branch_pad, branch->value);

            for (ASTNode *stmt = branch->next; stmt; stmt = stmt->next)
                st_gen_statement(ctx, out, max, stmt, indent + 8);
        }

        st_append(out, max, "%sEND_CASE;\n", pad);

    } else if (node->type == NODE_ACTION && node->var_name[0]) {
        if (strcmp(node->var_name, "ENTRY") == 0 || strcmp(node->var_name, "EXIT") == 0)
            st_gen_actions(ctx, out, max, node->next, indent);
        else
            st_gen_single_action(ctx, out, max, node, indent);
    } else if (node->type == NODE_ACTION) {
        st_gen_actions(ctx, out, max, node->next, indent);
    } else if (node->type == NODE_DOC_COMMENT) {
        st_append(out, max, "%s(* %s *)\n", pad, node->value);
    } else if (node->type == NODE_RECIPE_USE) {
        st_append(out, max,
                  "%s(* USE recipe/template: %s - expand via ATLAS IDE recipe library *)\n",
                  pad, node->var_name);
    } else if (node->type == NODE_CONST_DECL) {
        st_append(out, max, "%s(* CONST %s declared in VAR CONSTANT section *)\n",
                  pad, node->var_name);
    }
}
