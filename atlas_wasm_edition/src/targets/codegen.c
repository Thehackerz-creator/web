/**
 * codegen.c — Code Generator
 * Converts AST → IEC 61131-3 Structured Text (ST) or Ladder Logic comments
 * Supports: Siemens TIA Portal, CODESYS, Rockwell (Allen-Bradley)
 */

#include "plc_compiler.h"

/* ─── Output buffer ─────────────────────────────────────────────────────── */
#define OUTBUF_SIZE (1024 * 1024)   /* 1 MB initial, grows as needed */

typedef struct {
    char *buf;
    int   len;
    int   cap;
} OutBuf;

static void ob_init(OutBuf *ob) {
    ob->buf = (char *)malloc(OUTBUF_SIZE);
    ob->len = 0;
    ob->cap = OUTBUF_SIZE;
    ob->buf[0] = '\0';
}

static void ob_free(OutBuf *ob) { free(ob->buf); ob->buf = NULL; }

static void ob_append(OutBuf *ob, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (ob->len + needed + 2 >= ob->cap) {
        ob->cap = ob->len + needed + OUTBUF_SIZE;
        ob->buf = (char *)realloc(ob->buf, ob->cap);
    }
    va_start(args, fmt);
    ob->len += vsnprintf(ob->buf + ob->len, ob->cap - ob->len, fmt, args);
    va_end(args);
}

/* ─── Helpers ───────────────────────────────────────────────────────────── */
static const char *plc_target_name(PLCTarget t) {
    switch (t) {
        case PLC_SIEMENS_TIA:  return "Siemens TIA Portal (S7-1200/1500)";
        case PLC_CODESYS:      return "CODESYS V3 (IEC 61131-3)";
        case PLC_ROCKWELL_AOI: return "Rockwell Studio 5000 (AOI)";
        default:               return "Unknown";
    }
}

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
    return val; /* numeric or variable name */
}

/* Resolve variable name → its ST identifier */
static const char *resolve_st(CompilerCtx *ctx, const char *name) {
    if (strcasecmp(name, "ON")  == 0) return "TRUE";
    if (strcasecmp(name, "OFF") == 0) return "FALSE";
    Symbol *s = sym_lookup(ctx, name);
    return s ? s->st_name : name;
}

/* ─── Timer value → IEC time literal ────────────────────────────────────── */
static void timer_literal(CompilerCtx *ctx, ASTNode *node, char *out, int outlen) {
    (void)ctx;
    double ms = 0;
    switch (node->timer_unit) {
        case TIMER_SECONDS:      ms = node->timer_value * 1000.0; break;
        case TIMER_MINUTES:      ms = node->timer_value * 60000.0; break;
        case TIMER_MILLISECONDS: ms = node->timer_value; break;
    }
    snprintf(out, outlen, "T#%.0fms", ms);
}

static const char *const_type_for(const char *value) {
    if (strcasecmp(value, "ON") == 0 || strcasecmp(value, "OFF") == 0)
        return "BOOL";
    if (strchr(value, '.')) return "REAL";
    return "INT";
}

/* ─── Variable declaration section ─────────────────────────────────────── */
static void gen_var_declarations(CompilerCtx *ctx, OutBuf *ob) {
    ob_append(ob, "(* === Variable Declarations === *)\n");
    int const_count = 0;
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        if (s->is_used && s->kind == SYM_CONST) const_count++;
    }

    if (const_count > 0) {
        ob_append(ob, "VAR CONSTANT\n");
        for (int i = 0; i < ctx->sym_count; i++) {
            Symbol *s = &ctx->symbols[i];
            if (!s->is_used || s->kind != SYM_CONST) continue;
            const char *value = val_to_st(ctx, s->const_value);
            ob_append(ob, "    %s : %s := %s;%s%s%s\n",
                      s->st_name, const_type_for(s->const_value), value,
                      s->unit[0] ? "  (* unit: " : "",
                      s->unit[0] ? s->unit : "",
                      s->unit[0] ? " *)" : "");
        }
        ob_append(ob, "END_VAR\n\n");
    }

    ob_append(ob, "VAR\n");

    int timer_counter = 0;
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        if (!s->is_used) continue;
        if (s->kind == SYM_CONST) continue;

        if (s->kind == SYM_TIMER) {
            /* Timer instances */
            switch (ctx->target) {
                case PLC_SIEMENS_TIA:
                    ob_append(ob, "    %s AT %s : TON;  (* Timer On-Delay #%d *)\n",
                              s->st_name, s->plc_address, timer_counter++);
                    break;
                case PLC_CODESYS:
                    ob_append(ob, "    %s : TON;  (* Timer #%d *)\n",
                              s->st_name, timer_counter++);
                    break;
                case PLC_ROCKWELL_AOI:
                    ob_append(ob, "    %s : TIMER;  (* T4:%d *)\n",
                              s->st_name, timer_counter++);
                    break;
            }
            continue;
        }

        const char *type_str = (s->kind == SYM_REAL) ? "REAL" :
                               (s->kind == SYM_INT)  ? "INT"  : "BOOL";
        const char *dir_kw   = (s->direction == IO_INPUT) ? "VAR_INPUT" :
                               (s->direction == IO_OUTPUT)? "VAR_OUTPUT": "VAR";

        ob_append(ob, "    (* %s *) %s AT %s : %s;  (* %s%s%s%s *)\n",
                  dir_kw, s->st_name, s->plc_address, type_str,
                  s->direction == IO_INPUT  ? "Physical Input"  :
                  s->direction == IO_OUTPUT ? "Physical Output" : "Memory",
                  s->unit[0] ? "; unit: " : "",
                  s->unit[0] ? s->unit : "",
                  s->safety_critical ? "; safety-critical" :
                  s->safety_estop ? "; e-stop" : "");
    }
    ob_append(ob, "END_VAR\n\n");
}

/* ─── Forward declarations ──────────────────────────────────────────────── */
static void gen_condition_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int indent);
static void gen_actions_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *head, int indent);
static void gen_if_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int indent);

/* ─── Condition code generation ─────────────────────────────────────────── */
static void gen_condition_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int indent) {
    (void)indent;
    if (!node) return;

    switch (node->type) {
        case NODE_COMPARISON: {
            const char *var  = resolve_st(ctx, node->var_name);
            const char *op   = cmp_to_st(node->cmp_op);
            const char *val  = node->is_numeric ? node->value : val_to_st(ctx, node->value);
            ob_append(ob, "%s %s %s", var, op, val);
            break;
        }
        case NODE_AND:
            ob_append(ob, "(");
            gen_condition_st(ctx, ob, node->children[0], 0);
            ob_append(ob, " AND ");
            gen_condition_st(ctx, ob, node->children[1], 0);
            ob_append(ob, ")");
            break;
        case NODE_OR:
            ob_append(ob, "(");
            gen_condition_st(ctx, ob, node->children[0], 0);
            ob_append(ob, " OR ");
            gen_condition_st(ctx, ob, node->children[1], 0);
            ob_append(ob, ")");
            break;
        case NODE_NOT:
            ob_append(ob, "NOT(");
            gen_condition_st(ctx, ob, node->children[0], 0);
            ob_append(ob, ")");
            break;
        default: break;
    }
}

/* ─── Action code generation ────────────────────────────────────────────── */
static void gen_actions_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *head, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    for (ASTNode *act = head; act; act = act->next) {
        if (act->type != NODE_ACTION || !act->var_name[0]) continue;
        const char *var = resolve_st(ctx, act->var_name);
        const char *val = act->is_numeric ? act->value : val_to_st(ctx, act->value);
        ob_append(ob, "%s%s := %s;\n", pad, var, val);
    }
}

/* ─── Timer block generation ────────────────────────────────────────────── */
static void gen_timer_block(CompilerCtx *ctx, OutBuf *ob, ASTNode *node,
                             const char *cond_expr, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    char lit[32];
    timer_literal(ctx, node, lit, sizeof(lit));

    /* Use the synthetic timer bound to this IF node */
    const char *timer_st = "_TIMER_0";
    if (node->timer_symbol_index >= 0 && node->timer_symbol_index < ctx->sym_count) {
        if (ctx->symbols[node->timer_symbol_index].kind == SYM_TIMER) {
            timer_st = ctx->symbols[node->timer_symbol_index].st_name;
        }
    }

    switch (ctx->target) {
        case PLC_SIEMENS_TIA:
        case PLC_CODESYS:
            ob_append(ob, "%s(* Timer: activate TON for %s *)\n", pad, lit);
            ob_append(ob, "%s%s(IN := %s, PT := %s);\n", pad, timer_st, cond_expr, lit);
            ob_append(ob, "%s(* Use %s.Q (timer done) as actual condition *)\n",
                      pad, timer_st);
            break;
        case PLC_ROCKWELL_AOI:
            ob_append(ob, "%s(* Rockwell TON: %s, PRE=%s *)\n", pad, timer_st, lit);
            ob_append(ob, "%sTON(%s, %s, %s);\n", pad, timer_st, cond_expr, lit);
            break;
    }
}

/* ─── IF statement code generation ─────────────────────────────────────── */
static void gen_if_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    if (node->child_count < 2) return;

    ASTNode *cond       = node->children[0];
    ASTNode *then_node  = node->children[1];
    ASTNode *else_node  = (node->child_count > 2) ? node->children[2] : NULL;

    /* Build condition expression string */
    OutBuf cond_buf;
    ob_init(&cond_buf);
    gen_condition_st(ctx, &cond_buf, cond, 0);

    /* Timer wrapping */
    if (node->has_timer) {
        char lit[32];
        timer_literal(ctx, node, lit, sizeof(lit));
        ob_append(ob, "%s(* --- Timer-guarded IF [%s delay] --- *)\n", pad, lit);
        gen_timer_block(ctx, ob, node, cond_buf.buf, indent);

        /* Replace cond expression with timer.Q */
        char timer_q[MAX_IDENTIFIER_LEN + 8];
        const char *tn = "_TIMER_0";
        if (node->timer_symbol_index >= 0 && node->timer_symbol_index < ctx->sym_count) {
            if (ctx->symbols[node->timer_symbol_index].kind == SYM_TIMER) {
                tn = ctx->symbols[node->timer_symbol_index].st_name;
            }
        }
        switch (ctx->target) {
            case PLC_SIEMENS_TIA:
            case PLC_CODESYS:
                snprintf(timer_q, sizeof(timer_q), "%s.Q", tn); break;
            case PLC_ROCKWELL_AOI:
                snprintf(timer_q, sizeof(timer_q), "%s.DN", tn); break;
        }
        ob_append(ob, "%sIF %s THEN\n", pad, timer_q);
    } else {
        ob_append(ob, "%sIF %s THEN\n", pad, cond_buf.buf);
    }
    ob_free(&cond_buf);

    /* THEN actions */
    gen_actions_st(ctx, ob, then_node->next, indent + 4);

    /* ELSE actions */
    if (else_node) {
        ob_append(ob, "%sELSE\n", pad);
        gen_actions_st(ctx, ob, else_node->next, indent + 4);
    }

    ob_append(ob, "%sEND_IF;\n", pad);
}

/* ─── WHILE statement code generation (v2.0) ───────────────────────── */
static void gen_while_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    if (node->child_count < 2) return;

    ASTNode *cond = node->children[0];
    ASTNode *body = node->children[1];

    OutBuf cond_buf;
    ob_init(&cond_buf);
    gen_condition_st(ctx, &cond_buf, cond, 0);

    ob_append(ob, "%sWHILE %s DO\n", pad, cond_buf.buf);
    ob_free(&cond_buf);

    /* Body may contain IFs and assignments */
    for (ASTNode *act = body->next; act; act = act->next) {
        if (act->type == NODE_IF) {
            gen_if_st(ctx, ob, act, indent + 4);
        } else if (act->type == NODE_ACTION && act->var_name[0]) {
            const char *var = resolve_st(ctx, act->var_name);
            const char *val = act->is_numeric ? act->value : val_to_st(ctx, act->value);
            char ipad[64] = {0};
            for (int i = 0; i < indent + 4 && i < 60; i++) ipad[i] = ' ';
            ob_append(ob, "%s%s := %s;\n", ipad, var, val);
        }
    }

    ob_append(ob, "%sEND_WHILE;\n", pad);
}

/* ─── CASE statement code generation (v2.0) ────────────────────────── */
static void gen_case_st(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int indent) {
    char pad[64] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    const char *var = resolve_st(ctx, node->var_name);
    ob_append(ob, "%sCASE %s OF\n", pad, var);

    for (int i = 0; i < node->child_count; i++) {
        ASTNode *branch = node->children[i];
        if (!branch) continue;

        char ipad[64] = {0};
        for (int j = 0; j < indent + 4 && j < 60; j++) ipad[j] = ' ';

        if (strcasecmp(branch->value, "DEFAULT") == 0) {
            ob_append(ob, "%sELSE\n", ipad);
        } else {
            ob_append(ob, "%s%s:\n", ipad, branch->value);
        }

        /* Actions for this branch */
        char apad[64] = {0};
        for (int j = 0; j < indent + 8 && j < 60; j++) apad[j] = ' ';
        for (ASTNode *act = branch->next; act; act = act->next) {
            if (act->type != NODE_ACTION || !act->var_name[0]) continue;
            const char *avar = resolve_st(ctx, act->var_name);
            const char *aval = act->is_numeric ? act->value : val_to_st(ctx, act->value);
            ob_append(ob, "%s%s := %s;\n", apad, avar, aval);
        }
    }

    ob_append(ob, "%sEND_CASE;\n", pad);
}

/* ─── Struct generation ─────────────────────────────────────────────────── */
static void gen_struct(OutBuf *ob, ASTNode *node) {
    ob_append(ob, "TYPE %s :\nSTRUCT\n", node->var_name);
    for (int i = 0; i < node->child_count; i++) {
        ASTNode *decl = node->children[i];
        ob_append(ob, "    %s : BOOL;\n", decl->var_name);
    }
    ob_append(ob, "END_STRUCT\nEND_TYPE\n\n");
}

/* ─── Function Block generation ────────────────────────────────────────── */
static void gen_function_block(CompilerCtx *ctx, OutBuf *ob, ASTNode *node) {
    ob_append(ob, "FUNCTION_BLOCK %s\n", node->var_name);

    for (int i = 0; i < node->child_count; i++) {
        ASTNode *child = node->children[i];
        if (child->type == NODE_VAR_DECL) {
            const char *section = "VAR";
            if (strcmp(child->value, "INPUT") == 0) section = "VAR_INPUT";
            else if (strcmp(child->value, "OUTPUT") == 0) section = "VAR_OUTPUT";
            ob_append(ob, "%s\n", section);
            for (int j = 0; j < child->child_count; j++) {
                ob_append(ob, "    %s : BOOL;\n", child->children[j]->var_name);
            }
            ob_append(ob, "END_VAR\n");
        }
    }

    ob_append(ob, "\n(* Logic *)\n");
    for (int i = 0; i < node->child_count; i++) {
        ASTNode *child = node->children[i];
        if (child->type == NODE_IF) gen_if_st(ctx, ob, child, 4);
        else if (child->type == NODE_WHILE) gen_while_st(ctx, ob, child, 4);
        else if (child->type == NODE_CASE) gen_case_st(ctx, ob, child, 4);
        else if (child->type == NODE_CONTRACT) {
            OutBuf cond_buf; ob_init(&cond_buf);
            gen_condition_st(ctx, &cond_buf, child->children[0], 0);
            ob_append(ob, "    (* %s condition check: %s *)\n", child->var_name, cond_buf.buf);
            ob_free(&cond_buf);
        }
    }
    ob_append(ob, "END_FUNCTION_BLOCK\n\n");
}

/* ─── State Machine generation ────────────────────────────────────────── */
static void gen_state_machine(CompilerCtx *ctx, OutBuf *ob, ASTNode *node) {
    ob_append(ob, "(* State Machine: %s *)\n", node->var_name);
    ob_append(ob, "CASE %s_STATE OF\n", node->var_name);

    for (int i = 0; i < node->child_count; i++) {
        ASTNode *state = node->children[i];
        if (state->type != NODE_STATE) continue;
        ob_append(ob, "    %d: (* State: %s *)\n", i, state->var_name);

        for (int j = 0; j < state->child_count; j++) {
            ASTNode *child = state->children[j];
            if (child->type == NODE_ACTION) {
                gen_actions_st(ctx, ob, child->next, 8);
            } else if (child->type == NODE_TRANSITION) {
                OutBuf cond_buf; ob_init(&cond_buf);
                gen_condition_st(ctx, &cond_buf, child->children[0], 0);
                /* Find target state index */
                int target_idx = 0;
                for (int k = 0; k < node->child_count; k++) {
                    if (node->children[k]->type == NODE_STATE && strcmp(node->children[k]->var_name, child->var_name) == 0) {
                        target_idx = k; break;
                    }
                }
                ob_append(ob, "        IF %s THEN %s_STATE := %d; END_IF;\n", cond_buf.buf, node->var_name, target_idx);
                ob_free(&cond_buf);
            }
        }
    }
    ob_append(ob, "END_CASE;\n\n");
}

/* ─── Full ST program body ──────────────────────────────────────────────── */
static void gen_st_body(CompilerCtx *ctx, OutBuf *ob) {
    ASTNode *root = ctx->ast_root;
    ob_append(ob, "(* === Program Logic === *)\n\n");

    for (int i = 0; i < root->child_count; i++) {
        ASTNode *stmt = root->children[i];
        if (stmt->type == NODE_STRUCT || stmt->type == NODE_FUNCTION_BLOCK || stmt->type == NODE_STATE_MACHINE) {
            /* These still use their specific generators for now as they have complex wrappers */
            if (stmt->type == NODE_STRUCT) gen_struct(ob, stmt);
            else if (stmt->type == NODE_FUNCTION_BLOCK) gen_function_block(ctx, ob, stmt);
            else gen_state_machine(ctx, ob, stmt);
            continue;
        }
        
        char st_code[4096] = {0};
        st_gen_statement(ctx, st_code, sizeof(st_code), stmt, 0);
        if (st_code[0]) {
            ob_append(ob, "(* --- Statement %d [line %d] --- *)\n", i + 1, stmt->line);
            ob_append(ob, "%s\n", st_code);
        }
    }
}

/* ─── Ladder Logic (textual IL/comment representation) ──────────────────── */
static void gen_ladder_rung(CompilerCtx *ctx, OutBuf *ob, ASTNode *node, int rung_no) {
    if (node->type != NODE_IF || node->child_count < 2) return;

    ASTNode *cond      = node->children[0];
    ASTNode *then_node = node->children[1];
    ASTNode *else_node = (node->child_count > 2) ? node->children[2] : NULL;

    ob_append(ob, ";; RUNG %03d  [line %d]\n", rung_no, node->line);
    ob_append(ob, ";; ─────────────────────────────────────────\n");

    /* Condition contacts */
    if (cond->type == NODE_COMPARISON) {
        const char *var = resolve_st(ctx, cond->var_name);
        const char *op  = cmp_to_st(cond->cmp_op);
        const char *val = cond->is_numeric ? cond->value : val_to_st(ctx, cond->value);
        ob_append(ob, ";;  |--[%s %s %s]--|", var, op, val);
    } else if (cond->type == NODE_AND && cond->child_count == 2) {
        OutBuf lhs, rhs;
        ob_init(&lhs); ob_init(&rhs);
        gen_condition_st(ctx, &lhs, cond->children[0], 0);
        gen_condition_st(ctx, &rhs, cond->children[1], 0);
        ob_append(ob, ";;  |--[%s]--[%s]--|", lhs.buf, rhs.buf);
        ob_free(&lhs); ob_free(&rhs);
    } else if (cond->type == NODE_OR && cond->child_count == 2) {
        OutBuf lhs, rhs;
        ob_init(&lhs); ob_init(&rhs);
        gen_condition_st(ctx, &lhs, cond->children[0], 0);
        gen_condition_st(ctx, &rhs, cond->children[1], 0);
        ob_append(ob, ";;  |--[%s]--+\n", lhs.buf);
        ob_append(ob, ";;           +--[%s]--|", rhs.buf);
        ob_free(&lhs); ob_free(&rhs);
    }

    if (node->has_timer) {
        char lit[32]; timer_literal(ctx, node, lit, sizeof(lit));
        const char *timer_st = "_TIMER_0";
        if (node->timer_symbol_index >= 0 && node->timer_symbol_index < ctx->sym_count) {
            if (ctx->symbols[node->timer_symbol_index].kind == SYM_TIMER) {
                timer_st = ctx->symbols[node->timer_symbol_index].st_name;
            }
        }
        if (ctx->target == PLC_ROCKWELL_AOI) {
            ob_append(ob, "--[TON %s PT=%s]", timer_st, lit);
        } else {
            ob_append(ob, "--[TON %s PT=%s]", timer_st, lit);
        }
    }

    /* THEN coil */
    for (ASTNode *act = then_node->next; act; act = act->next) {
        if (!act->var_name[0]) continue;
        const char *var = resolve_st(ctx, act->var_name);
        const char *val = val_to_st(ctx, act->value);
        if (strcmp(val, "TRUE") == 0)
            ob_append(ob, "--( %s )--|\n", var);
        else if (strcmp(val, "FALSE") == 0)
            ob_append(ob, "--(/%s/)--|\n", var);  /* normally-closed coil */
        else
            ob_append(ob, "--[MOV %s->%s]--|\n", val, var);
    }

    /* ELSE coil (separate rung with negated condition) */
    if (else_node) {
        ob_append(ob, ";;\n");
        ob_append(ob, ";; RUNG %03d-E (ELSE branch — inverted condition)\n", rung_no);
        ob_append(ob, ";;  |--[NOT condition above]--|");
        for (ASTNode *act = else_node->next; act; act = act->next) {
            if (!act->var_name[0]) continue;
            const char *var = resolve_st(ctx, act->var_name);
            const char *val = val_to_st(ctx, act->value);
            if (strcmp(val, "TRUE") == 0)
                ob_append(ob, "--( %s )--|\n", var);
            else
                ob_append(ob, "--(/%s/)--|\n", var);
        }
    }
    ob_append(ob, ";;\n\n");
}

/* ─── Platform-specific file header ─────────────────────────────────────── */
static void gen_header(CompilerCtx *ctx, OutBuf *ob, const char *filename) {
    char ts[64];
    if (ctx->deterministic_output) {
        strncpy(ts, "1970-01-01 00:00:00 UTC", sizeof(ts));
        ts[sizeof(ts) - 1] = '\0';
    } else {
        time_t t = time(NULL);
        struct tm *tm_info = localtime(&t);
        if (tm_info)
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
        else
            strncpy(ts, "unknown", sizeof(ts));
        ts[sizeof(ts) - 1] = '\0';
    }

    if (ctx->fmt == FMT_STRUCTURED_TEXT) {
        ob_append(ob, "(* ============================================================= *)\n");
        ob_append(ob, "(*  AUTO-GENERATED BY PLC DSL COMPILER v2.0                    *)\n");
        ob_append(ob, "(*  Target  : %-48s*)\n", plc_target_name(ctx->target));
        ob_append(ob, "(*  CPU     : %-48s*)\n", cpu_arch_name(ctx->cpu_arch));
        ob_append(ob, "(*  ABI     : %d-bit, %s-endian%-31s*)\n",
                  cpu_arch_bits(ctx->cpu_arch), cpu_arch_endian(ctx->cpu_arch), "");
        ob_append(ob, "(*  Harden  : level %-41d*)\n", ctx->hardening_level);
        ob_append(ob, "(*  Format  : IEC 61131-3 Structured Text                       *)\n");
        ob_append(ob, "(*  Date    : %-48s*)\n", ts);
        ob_append(ob, "(*  Source  : %-48s*)\n", filename);
        ob_append(ob, "(*  WARNING : Do NOT edit manually — regenerate from DSL source  *)\n");
        ob_append(ob, "(* ============================================================= *)\n\n");

        /* Platform-specific program block wrapper */
        switch (ctx->target) {
            case PLC_SIEMENS_TIA:
                ob_append(ob, "ORGANIZATION_BLOCK OB1\n");
                ob_append(ob, "TITLE = 'Main Program'\n");
                ob_append(ob, "VERSION : '0.1'\n\n");
                break;
            case PLC_CODESYS:
                ob_append(ob, "PROGRAM PLC_PRG\n\n");
                break;
            case PLC_ROCKWELL_AOI:
                ob_append(ob, "(* Add-On Instruction (AOI) Block *)\n");
                ob_append(ob, "ROUTINE MainRoutine\n\n");
                break;
        }
    } else {
        ob_append(ob, ";; ============================================================\n");
        ob_append(ob, ";; AUTO-GENERATED LADDER LOGIC — PLC DSL COMPILER v2.0\n");
        ob_append(ob, ";; Target : %s\n", plc_target_name(ctx->target));
        ob_append(ob, ";; CPU    : %s (%d-bit, %s-endian)\n",
                  cpu_arch_name(ctx->cpu_arch), cpu_arch_bits(ctx->cpu_arch),
                  cpu_arch_endian(ctx->cpu_arch));
        ob_append(ob, ";; Harden : level %d\n", ctx->hardening_level);
        ob_append(ob, ";; Date   : %s\n", ts);
        ob_append(ob, ";; Source : %s\n", filename);
        ob_append(ob, ";; Note   : Import into TIA Portal / CODESYS / Studio 5000\n");
        ob_append(ob, ";; ============================================================\n\n");
    }
}

/* ─── Platform-specific file footer ─────────────────────────────────────── */
static void gen_footer(CompilerCtx *ctx, OutBuf *ob) {
    if (ctx->fmt == FMT_STRUCTURED_TEXT) {
        switch (ctx->target) {
            case PLC_SIEMENS_TIA:
                ob_append(ob, "\nEND_ORGANIZATION_BLOCK\n");
                break;
            case PLC_CODESYS:
                ob_append(ob, "\nEND_PROGRAM\n");
                break;
            case PLC_ROCKWELL_AOI:
                ob_append(ob, "\nEND_ROUTINE\n");
                break;
        }
    } else {
        ob_append(ob, ";; END OF LADDER PROGRAM\n");
    }
    ob_append(ob, "\n(* Compiled: %d symbols, %d nodes *)\n",
              ctx->sym_count, ctx->nodes_allocated);
}

/* ─── Main code generation entry ────────────────────────────────────────── */
int codegen_run(CompilerCtx *ctx, const char *out_path) {
    log_info(ctx, "CodeGen: generating %s for %s",
             ctx->fmt == FMT_STRUCTURED_TEXT ? "Structured Text" : "Ladder Logic",
             plc_target_name(ctx->target));

    OutBuf ob;
    ob_init(&ob);

    gen_header(ctx, &ob, out_path);
    gen_var_declarations(ctx, &ob);

    if (ctx->fmt == FMT_STRUCTURED_TEXT) {
        gen_st_body(ctx, &ob);
    } else {
        ob_append(&ob, "(* LADDER LOGIC PROGRAM BODY *)\n\n");
        int rung = 1;
        for (int i = 0; i < ctx->ast_root->child_count; i++) {
            ASTNode *stmt = ctx->ast_root->children[i];
            if (stmt->type == NODE_IF)
                gen_ladder_rung(ctx, &ob, stmt, rung++);
        }
    }

    gen_footer(ctx, &ob);

    /* Write to file */
    if (compiler_write_output(ctx, out_path, ob.buf)) {
        log_info(ctx, "CodeGen: output written to '%s' (%d bytes)", out_path, ob.len);
    } else {
        log_error(ctx, 0, "CodeGen: failed to write output file '%s'", out_path);
        ob_free(&ob);
        return 0;
    }

    /* Print to console */
    if (ctx->print_generated_output && !ctx->json_mode) {
        printf("\n%s\n", ob.buf);
    }

    ob_free(&ob);
    return 1;
}
