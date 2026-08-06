/**
 * parser.c — Recursive-descent parser → Universal AST
 * Grammar:
 *   program        := statement*
 *   statement      := if_stmt | assignment_stmt
 *   if_stmt        := IF condition [FOR number time_unit] THEN action_list [ELSE action_list] END
 *   condition      := simple_cond ((AND|OR) simple_cond)*
 *   simple_cond    := [NOT] IDENTIFIER comparator value
 *   comparator     := = | != | >= | <= | > | <
 *   value          := ON | OFF | NUMBER | IDENTIFIER
 *   action_list    := action+
 *   action         := IDENTIFIER = (ON | OFF | NUMBER | IDENTIFIER)
 *   time_unit      := SECONDS | MINUTES | MILLISECONDS
 */

#include "plc_compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Forward declarations ──────────────────────────────────────────────── */
static ASTNode *parse_statement(CompilerCtx *ctx);
static ASTNode *parse_if(CompilerCtx *ctx);
static ASTNode *parse_action(CompilerCtx *ctx);

/* ─── Token access helpers ──────────────────────────────────────────────── */
static Token *cur(CompilerCtx *ctx) {
    return &ctx->tokens[ctx->token_pos];
}

static Token *peek_tok(CompilerCtx *ctx, int offset) {
    int idx = ctx->token_pos + offset;
    if (idx >= ctx->token_count) return &ctx->tokens[ctx->token_count - 1];
    return &ctx->tokens[idx];
}

static Token *consume(CompilerCtx *ctx) {
    Token *t = &ctx->tokens[ctx->token_pos];
    if (ctx->token_pos < ctx->token_count - 1) ctx->token_pos++;
    return t;
}

static int expect(CompilerCtx *ctx, PlcTokenType tt) {
    if (cur(ctx)->type == tt) { consume(ctx); return 1; }
    log_error(ctx, cur(ctx)->line, "Expected '%s' but got '%s' ('%s')",
              token_type_name(tt), token_type_name(cur(ctx)->type), cur(ctx)->value);
    return 0;
}

/* ─── AST node factory ──────────────────────────────────────────────────── */
static ASTNode *node_new(CompilerCtx *ctx, NodeType type, int line) {
    ASTNode *n = NULL;
    if (ctx && ctx->max_ast_nodes > 0 && ctx->nodes_allocated >= ctx->max_ast_nodes) {
        log_error(ctx, line, "AST node limit exceeded (%d)", ctx->max_ast_nodes);
        return NULL;
    }
    if (ctx && ctx->use_arena_gc) {
        n = (ASTNode *)gc_arena_alloc(&ctx->arena, sizeof(ASTNode), sizeof(void *));
    } else {
        n = (ASTNode *)calloc(1, sizeof(ASTNode));
    }
    if (!n) {
        log_error(ctx, line, "Out of memory allocating AST node");
        return NULL;
    }
    n->type = type;
    n->line = line;
    n->timer_symbol_index = -1;
    ctx->nodes_allocated++;
    return n;
}

static void node_add_child(ASTNode *parent, ASTNode *child) {
    if (parent && child && parent->child_count < MAX_CHILDREN)
        parent->children[parent->child_count++] = child;
}

static void node_copy_text(char dst[MAX_IDENTIFIER_LEN], const char *src) {
    snprintf(dst, MAX_IDENTIFIER_LEN, "%s", src ? src : "");
}

static int is_const_identifier(const char *name) {
    int has_alpha = 0;
    int has_lower = 0;
    if (!name || !name[0]) return 0;
    for (int i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalpha(c)) {
            has_alpha = 1;
            if (islower(c)) has_lower = 1;
        } else if (!isdigit(c) && c != '_') {
            return 0;
        }
    }
    return has_alpha && !has_lower;
}

/* ─── Apply @annotation to a symbol ─────────────────────────────────────── */
static void apply_annotation(CompilerCtx *ctx, const char *var_name, const char *annotation) {
    Symbol *s = sym_lookup(ctx, var_name);
    if (!s) return; /* symbol may not exist yet during parse; semantic pass will catch */

    if (strcmp(annotation, "CRITICAL") == 0 || strcmp(annotation, "SAFETY") == 0)
        s->safety_critical = 1;
    else if (strcmp(annotation, "ESTOP") == 0)
        s->safety_estop = 1;
    else if (strcmp(annotation, "SIL1") == 0)
        s->safety_sil_level = 1;
    else if (strcmp(annotation, "SIL2") == 0)
        s->safety_sil_level = 2;
    else if (strcmp(annotation, "SIL3") == 0)
        s->safety_sil_level = 3;
    else if (strcmp(annotation, "SIL4") == 0)
        s->safety_sil_level = 4;
}

static void apply_annotation_to_node(ASTNode *node, const char *annotation) {
    if (!node || !annotation) return;

    if (strcmp(annotation, "CRITICAL") == 0 || strcmp(annotation, "SAFETY") == 0)
        node->safety_critical = 1;
    else if (strcmp(annotation, "ESTOP") == 0)
        node->safety_estop = 1;
    else if (strcmp(annotation, "SIL1") == 0)
        node->safety_sil_level = 1;
    else if (strcmp(annotation, "SIL2") == 0)
        node->safety_sil_level = 2;
    else if (strcmp(annotation, "SIL3") == 0)
        node->safety_sil_level = 3;
    else if (strcmp(annotation, "SIL4") == 0)
        node->safety_sil_level = 4;
}

/* ─── Consume any trailing @annotations and store on node ────────────────── */
static void parse_annotations(CompilerCtx *ctx, ASTNode *node) {
    while (cur(ctx)->type == TOK_ANNOTATION || cur(ctx)->type == TOK_AT) {
        if (cur(ctx)->type == TOK_AT) {
            log_warn(ctx, "Unknown annotation '%s' on line %d — ignored",
                     cur(ctx)->value, cur(ctx)->line);
            consume(ctx);
            continue;
        }
        /* Store annotation value on the node for semantic pass to apply */
        char ann[MAX_IDENTIFIER_LEN];
        snprintf(ann, sizeof(ann), "%s", cur(ctx)->value);
        consume(ctx);

        /* If node has a var_name, apply immediately if symbol exists */
        if (node && node->var_name[0]) {
            apply_annotation(ctx, node->var_name, ann);
            apply_annotation_to_node(node, ann);
        }
    }
}


void ast_free(CompilerCtx *ctx, ASTNode *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; i++)
        ast_free(ctx, node->children[i]);
    /* free linked action list */
    ASTNode *act = node->next;
    while (act) {
        ASTNode *tmp = act->next;
        if (!(ctx && ctx->use_arena_gc)) {
            free(act);
        }
        ctx->nodes_freed++;
        act = tmp;
    }
    if (!(ctx && ctx->use_arena_gc)) {
        free(node);
    }
    ctx->nodes_freed++;
}

/* ─── Parse comparator ──────────────────────────────────────────────────── */
static CmpOp parse_cmp_op(CompilerCtx *ctx) {
    PlcTokenType tt = cur(ctx)->type;
    consume(ctx);
    switch (tt) {
        case TOK_EQ:  return CMP_EQ;
        case TOK_NEQ: return CMP_NEQ;
        case TOK_GTE: return CMP_GTE;
        case TOK_LTE: return CMP_LTE;
        case TOK_GT:  return CMP_GT;
        case TOK_LT:  return CMP_LT;
        default:
            log_error(ctx, cur(ctx)->line, "Expected comparator, got '%s'", cur(ctx)->value);
            return CMP_EQ;
    }
}

static int is_comparator(PlcTokenType tt) {
    return tt == TOK_EQ  || tt == TOK_NEQ || tt == TOK_GTE ||
           tt == TOK_LTE || tt == TOK_GT  || tt == TOK_LT;
}

/* ─── Parse a single value (ON/OFF/number/identifier) ─────────────────── */
static void parse_value_into(CompilerCtx *ctx, ASTNode *node) {
    Token *t = cur(ctx);
    if (t->type == TOK_ON) {
        node_copy_text(node->value, "ON");
        node->is_numeric = 0;
        consume(ctx);
    } else if (t->type == TOK_OFF) {
        node_copy_text(node->value, "OFF");
        node->is_numeric = 0;
        consume(ctx);
    } else if (t->type == TOK_NUMBER) {
        node_copy_text(node->value, t->value);
        node->numeric_val = safe_atof(t->value);
        node->is_numeric  = 1;
        consume(ctx);
    } else if (t->type == TOK_IDENTIFIER || t->type == TOK_STRING_LITERAL) {
        node_copy_text(node->value, t->value);
        node->is_numeric = 0;
        consume(ctx);
    } else {
        log_error(ctx, t->line, "Expected value (ON/OFF/number/var), got '%s'", t->value);
        consume(ctx);
    }
}

static void parse_optional_unit(CompilerCtx *ctx, ASTNode *node) {
    if (cur(ctx)->type != TOK_IN) return;
    consume(ctx);
    if (cur(ctx)->type == TOK_IDENTIFIER || cur(ctx)->type == TOK_STRING_LITERAL) {
        node_copy_text(node->unit, cur(ctx)->value);
        consume(ctx);
    } else {
        log_error(ctx, cur(ctx)->line, "Expected unit name after IN, got '%s'", cur(ctx)->value);
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
    }
}

/* ─── Parse simple condition: IDENT comparator value ─────────────── */
static ASTNode *parse_simple_condition(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    ASTNode *cond = node_new(ctx, NODE_COMPARISON, line);
    if (!cond) {
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
        return NULL;
    }

    /* Variable name */
    if (cur(ctx)->type != TOK_IDENTIFIER) {
        log_error(ctx, line, "Expected variable name in condition, got '%s'", cur(ctx)->value);
        consume(ctx);
        return cond;
    }
    node_copy_text(cond->var_name, cur(ctx)->value);
    consume(ctx);

    /* Comparator */
    if (is_comparator(cur(ctx)->type)) {
        cond->cmp_op = parse_cmp_op(ctx);
    } else {
        log_error(ctx, line, "Expected comparator after '%s', got '%s'",
                  cond->var_name, cur(ctx)->value);
        consume(ctx);
    }

    /* Value */
    parse_value_into(ctx, cond);

    return cond;
}

/* ─── Forward declaration for recursion ─── */
static ASTNode *parse_condition(CompilerCtx *ctx);
static ASTNode *parse_condition_depth(CompilerCtx *ctx, int depth);

static int condition_stop_token(PlcTokenType tt) {
    return tt == TOK_THEN || tt == TOK_DO || tt == TOK_UNTIL ||
           tt == TOK_COLON || tt == TOK_SEMICOLON || tt == TOK_ELSE ||
           tt == TOK_END || tt == TOK_END_CASE || tt == TOK_RPAREN ||
           tt == TOK_EOF;
}

static ASTNode *parse_error_condition(CompilerCtx *ctx, int line) {
    ASTNode *cond = node_new(ctx, NODE_COMPARISON, line);
    if (!cond) return NULL;
    node_copy_text(cond->var_name, "__parse_error");
    node_copy_text(cond->value, "OFF");
    cond->cmp_op = CMP_EQ;
    return cond;
}

static ASTNode *condition_depth_error(CompilerCtx *ctx, int depth) {
    int line = cur(ctx)->line;
    log_error(ctx, line,
              "Condition nesting depth exceeded (%d > %d)",
              depth, MAX_PARSE_CONDITION_DEPTH);
    while (!condition_stop_token(cur(ctx)->type))
        consume(ctx);
    return parse_error_condition(ctx, line);
}

/* ─── Parse primary condition: (cond) | NOT cond | simple_cond ─── */
static ASTNode *parse_primary_condition_depth(CompilerCtx *ctx, int depth) {
    int line = cur(ctx)->line;
    if (depth > MAX_PARSE_CONDITION_DEPTH)
        return condition_depth_error(ctx, depth);

    if (cur(ctx)->type == TOK_LPAREN) {
        consume(ctx);
        ASTNode *node = parse_condition_depth(ctx, depth + 1);
        expect(ctx, TOK_RPAREN);
        return node;
    }
    if (cur(ctx)->type == TOK_NOT) {
        consume(ctx);
        ASTNode *not_node = node_new(ctx, NODE_NOT, line);
        if (!not_node) return NULL;
        node_add_child(not_node, parse_primary_condition_depth(ctx, depth + 1));
        return not_node;
    }
    return parse_simple_condition(ctx);
}

/* ─── Parse condition chain: cond (AND/OR cond)* ─────────────────────── */
static ASTNode *parse_condition_depth(CompilerCtx *ctx, int depth) {
    if (depth > MAX_PARSE_CONDITION_DEPTH)
        return condition_depth_error(ctx, depth);

    ASTNode *left = parse_primary_condition_depth(ctx, depth);
    if (!left) return NULL;

    while (cur(ctx)->type == TOK_AND || cur(ctx)->type == TOK_OR) {
        int     line = cur(ctx)->line;
        NodeType op  = (cur(ctx)->type == TOK_AND) ? NODE_AND : NODE_OR;
        consume(ctx);

        ASTNode *compound = node_new(ctx, op, line);
        if (!compound) return left;
        node_add_child(compound, left);
        node_add_child(compound, parse_primary_condition_depth(ctx, depth + 1));
        left = compound;
    }
    return left;
}

static ASTNode *parse_condition(CompilerCtx *ctx) {
    return parse_condition_depth(ctx, 0);
}

/* ─── Parse optional timer: FOR number (SECONDS|MINUTES|MS) ────────────── */
static void parse_timer(CompilerCtx *ctx, ASTNode *if_node) {
    if (cur(ctx)->type != TOK_FOR) return;
    consume(ctx); /* eat FOR */

    if (cur(ctx)->type != TOK_NUMBER) {
        log_error(ctx, cur(ctx)->line, "Expected number after FOR, got '%s'", cur(ctx)->value);
        return;
    }
    if_node->timer_value = safe_atof(cur(ctx)->value);
    consume(ctx);

    if (cur(ctx)->type == TOK_SECONDS) {
        if_node->timer_unit = TIMER_SECONDS;
        consume(ctx);
    } else if (cur(ctx)->type == TOK_MINUTES) {
        if_node->timer_unit = TIMER_MINUTES;
        consume(ctx);
    } else if (cur(ctx)->type == TOK_MILLISECONDS) {
        if_node->timer_unit = TIMER_MILLISECONDS;
        consume(ctx);
    } else {
        log_warn(ctx, "No time unit after FOR value — assuming SECONDS");
        if_node->timer_unit = TIMER_SECONDS;
    }
    if_node->has_timer = 1;
}

/* ─── Parse one action: IDENT = (ON|OFF|number|ident) ─────────────────── */
static ASTNode *parse_action(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    ASTNode *act = node_new(ctx, NODE_ACTION, line);
    if (!act) {
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
        return NULL;
    }

    if (cur(ctx)->type != TOK_IDENTIFIER) {
        log_error(ctx, line, "Expected output variable in action, got '%s'", cur(ctx)->value);
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
        return act;
    }
    node_copy_text(act->var_name, cur(ctx)->value);
    consume(ctx);

    if (cur(ctx)->type != TOK_EQ) {
        log_error(ctx, line, "Expected '=' in action after '%s', got '%s'",
                  act->var_name, cur(ctx)->value);
    } else consume(ctx);

    parse_value_into(ctx, act);
    return act;
}

/* ─── Parse action list (stops at ELSE, END, or EOF) ───────────────────── */
static ASTNode *parse_action_list(CompilerCtx *ctx) {
    ASTNode *head = NULL, *tail = NULL;

    while (cur(ctx)->type != TOK_ELSE &&
           cur(ctx)->type != TOK_END  &&
           cur(ctx)->type != TOK_EOF) {

        ASTNode *act = parse_action(ctx);
        /* Optional semicolon between actions */
        if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);

        if (act) {
            if (!head) { head = tail = act; }
            else        { tail->next = act; tail = act; }
        }

        /* Another IF nested inside action list */
        if (cur(ctx)->type == TOK_IF) break;
    }
    return head;
}

/* ─── Parse IF statement ───────────────────────────────────────────────── */
static ASTNode *parse_if(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_IF);

    ASTNode *if_node = node_new(ctx, NODE_IF, line);
    if (!if_node) return NULL;

    /* [0] = condition */
    ASTNode *cond = parse_condition(ctx);
    node_add_child(if_node, cond);

    /* Optional: FOR n SECONDS */
    parse_timer(ctx, if_node);

    /* THEN */
    if (!expect(ctx, TOK_THEN)) {
        log_error(ctx, line, "Missing THEN in IF statement");
    }

    /* [1] = then-branch wrapper */
    ASTNode *then_branch = node_new(ctx, NODE_ACTION, line);
    if (then_branch) then_branch->next = parse_action_list(ctx);
    node_add_child(if_node, then_branch);

    /* Optional ELSE */
    if (cur(ctx)->type == TOK_ELSE) {
        consume(ctx);
        ASTNode *else_branch = node_new(ctx, NODE_ACTION, line);
        if (else_branch) else_branch->next = parse_action_list(ctx);
        node_add_child(if_node, else_branch);
    }

    /* END */
    if (!expect(ctx, TOK_END)) {
        log_error(ctx, line, "Missing END for IF at line %d", line);
    }

    return if_node;
}

/* ─── Parse WHILE statement (v2.0) ───────────────────────────────────── */
static ASTNode *parse_while(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_WHILE);

    ASTNode *while_node = node_new(ctx, NODE_WHILE, line);
    if (!while_node) return NULL;

    /* [0] = condition */
    ASTNode *cond = parse_condition(ctx);
    node_add_child(while_node, cond);

    /* DO */
    if (!expect(ctx, TOK_DO)) {
        log_error(ctx, line, "Expected DO after WHILE condition");
    }

    /* [1] = body (action list, may contain nested IFs) */
    ASTNode *body = node_new(ctx, NODE_ACTION, line);
    ASTNode *head = NULL, *tail = NULL;
    while (cur(ctx)->type != TOK_END && cur(ctx)->type != TOK_EOF) {
        ASTNode *stmt = NULL;
        if (cur(ctx)->type == TOK_IF) {
            stmt = parse_if(ctx);
        } else {
            stmt = parse_action(ctx);
            if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
        }
        if (stmt) {
            if (!head) { head = tail = stmt; }
            else       { tail->next = stmt; tail = stmt; }
        }
    }
    if (!head)
        log_warn(ctx, "WHILE at line %d has an empty body", line);
    if (body) body->next = head;
    node_add_child(while_node, body);

    if (!expect(ctx, TOK_END)) {
        log_error(ctx, line, "Missing END for WHILE at line %d", line);
    }

    return while_node;
}

/* ─── Parse CASE statement (v2.0) ────────────────────────────────────── */
static ASTNode *parse_case(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_CASE);

    ASTNode *case_node = node_new(ctx, NODE_CASE, line);
    if (!case_node) return NULL;

    /* Variable to switch on */
    if (cur(ctx)->type != TOK_IDENTIFIER) {
        log_error(ctx, line, "Expected variable name after CASE, got '%s'", cur(ctx)->value);
    } else {
        node_copy_text(case_node->var_name, cur(ctx)->value);
        consume(ctx);
    }

    /* OF */
    if (!expect(ctx, TOK_OF)) {
        log_error(ctx, line, "Expected OF after CASE variable");
    }

    /* Parse branches: value COLON action_list */
    while (cur(ctx)->type != TOK_END_CASE && cur(ctx)->type != TOK_END &&
           cur(ctx)->type != TOK_EOF) {
        int branch_line = cur(ctx)->line;
        ASTNode *branch = node_new(ctx, NODE_CASE_BRANCH, branch_line);
        if (!branch) {
            if (cur(ctx)->type != TOK_EOF) consume(ctx);
            continue;
        }

        if (cur(ctx)->type == TOK_DEFAULT) {
            node_copy_text(branch->value, "DEFAULT");
            consume(ctx);
        } else if (cur(ctx)->type == TOK_NUMBER || cur(ctx)->type == TOK_IDENTIFIER ||
                   cur(ctx)->type == TOK_ON || cur(ctx)->type == TOK_OFF) {
            node_copy_text(branch->value, cur(ctx)->value);
            if (cur(ctx)->type == TOK_NUMBER) {
                branch->numeric_val = safe_atof(cur(ctx)->value);
                branch->is_numeric = 1;
            }
            consume(ctx);
        } else {
            log_error(ctx, branch_line, "Expected case value or DEFAULT, got '%s'", cur(ctx)->value);
            consume(ctx);
            continue;
        }

        /* Colon separator */
        if (cur(ctx)->type == TOK_COLON) consume(ctx);

        /* Actions for this branch */
        ASTNode *act_head = NULL, *act_tail = NULL;
        while (cur(ctx)->type != TOK_NUMBER && cur(ctx)->type != TOK_DEFAULT &&
               cur(ctx)->type != TOK_END_CASE && cur(ctx)->type != TOK_END &&
               cur(ctx)->type != TOK_EOF) {
            /* Stop if we see what looks like a new branch */
            if ((cur(ctx)->type == TOK_IDENTIFIER || cur(ctx)->type == TOK_ON ||
                 cur(ctx)->type == TOK_OFF) &&
                peek_tok(ctx, 1)->type == TOK_COLON) break;

            ASTNode *act = parse_action(ctx);
            if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
            if (act) {
                if (!act_head) { act_head = act_tail = act; }
                else           { act_tail->next = act; act_tail = act; }
            }
        }
        branch->next = act_head;
        node_add_child(case_node, branch);
    }

    /* END_CASE or END */
    if (cur(ctx)->type == TOK_END_CASE) consume(ctx);
    else if (!expect(ctx, TOK_END)) {
        log_error(ctx, line, "Missing END_CASE for CASE at line %d", line);
    }

    return case_node;
}

/* ─── Parse variable declaration: name [: type] ───────────────────────── */
static ASTNode *parse_var_decl(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    ASTNode *n = node_new(ctx, NODE_VAR_DECL, line);
    if (!n) {
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
        return NULL;
    }

    if (cur(ctx)->type != TOK_IDENTIFIER) {
        log_error(ctx, line, "Expected variable name, got '%s' ('%s')", 
                  token_type_name(cur(ctx)->type), cur(ctx)->value);
        consume(ctx);
        return n;
    }
    node_copy_text(n->var_name, cur(ctx)->value);
    consume(ctx);

    /* Optional type: : TYPE */
    if (cur(ctx)->type == TOK_COLON) {
        consume(ctx);
        if (cur(ctx)->type == TOK_IDENTIFIER) {
            node_copy_text(n->value, cur(ctx)->value);
            consume(ctx);
        } else {
            log_error(ctx, line, "Expected type name after ':', got '%s'", cur(ctx)->value);
            consume(ctx);
        }
    } else if (cur(ctx)->type == TOK_IN) {
        node_copy_text(n->value, "REAL");
    }
    parse_optional_unit(ctx, n);
    parse_annotations(ctx, n);
    return n;
}

/* ─── Parse Struct ──────────────────────────────────────────────────────── */
static ASTNode *parse_struct(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_STRUCT);

    ASTNode *n = node_new(ctx, NODE_STRUCT, line);
    if (!n) return NULL;
    if (cur(ctx)->type == TOK_IDENTIFIER) {
        node_copy_text(n->var_name, cur(ctx)->value);
        consume(ctx);
    }

    while (cur(ctx)->type != TOK_END_STRUCT && cur(ctx)->type != TOK_EOF) {
        node_add_child(n, parse_var_decl(ctx));
        if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
    }
    expect(ctx, TOK_END_STRUCT);
    return n;
}

/* ─── Parse Function Block ────────────────────────────────────────────── */
static ASTNode *parse_function_block(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_FUNCTION_BLOCK);

    ASTNode *n = node_new(ctx, NODE_FUNCTION_BLOCK, line);
    if (!n) return NULL;
    if (cur(ctx)->type == TOK_IDENTIFIER) {
        node_copy_text(n->var_name, cur(ctx)->value);
        consume(ctx);
    }

    while (cur(ctx)->type != TOK_END_FUNCTION_BLOCK && cur(ctx)->type != TOK_EOF) {
        if (cur(ctx)->type == TOK_VAR_INPUT || cur(ctx)->type == TOK_VAR_OUTPUT || cur(ctx)->type == TOK_VAR) {
            PlcTokenType section = consume(ctx)->type;
            ASTNode *sec_node = node_new(ctx, NODE_VAR_DECL, cur(ctx)->line);
            if (!sec_node) continue;
            if (section == TOK_VAR_INPUT) strncpy(sec_node->value, "INPUT", 15);
            else if (section == TOK_VAR_OUTPUT) strncpy(sec_node->value, "OUTPUT", 15);
            else strncpy(sec_node->value, "LOCAL", 15);

            while (cur(ctx)->type != TOK_END_VAR && cur(ctx)->type != TOK_EOF) {
                if (cur(ctx)->type == TOK_VAR || cur(ctx)->type == TOK_VAR_INPUT || 
                    cur(ctx)->type == TOK_VAR_OUTPUT) break; /* nested or new section */
                node_add_child(sec_node, parse_var_decl(ctx));
                if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
                if (cur(ctx)->type == TOK_IF || cur(ctx)->type == TOK_WHILE) break; /* logic started */
            }
            node_add_child(n, sec_node);
            if (cur(ctx)->type == TOK_END_VAR) consume(ctx);
        } else if (cur(ctx)->type == TOK_PRE || cur(ctx)->type == TOK_POST || cur(ctx)->type == TOK_INVARIANT) {
            ASTNode *c = node_new(ctx, NODE_CONTRACT, cur(ctx)->line);
            if (!c) {
                consume(ctx);
                continue;
            }
            if (cur(ctx)->type == TOK_PRE) strncpy(c->var_name, "PRE", 15);
            else if (cur(ctx)->type == TOK_POST) strncpy(c->var_name, "POST", 15);
            else strncpy(c->var_name, "INVARIANT", 15);
            consume(ctx);
            node_add_child(c, parse_condition(ctx));
            node_add_child(n, c);
        } else {
            /* Logic */
            ASTNode *stmt = parse_statement(ctx);
            if (stmt) node_add_child(n, stmt);
        }
    }
    expect(ctx, TOK_END_FUNCTION_BLOCK);
    return n;
}

/* ─── Parse State Machine ────────────────────────────────────────────── */
static ASTNode *parse_state_machine(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_STATE_MACHINE);

    ASTNode *n = node_new(ctx, NODE_STATE_MACHINE, line);
    if (!n) return NULL;
    if (cur(ctx)->type == TOK_IDENTIFIER) {
        node_copy_text(n->var_name, cur(ctx)->value);
        consume(ctx);
    }

    while (cur(ctx)->type != TOK_END_STATE_MACHINE && cur(ctx)->type != TOK_EOF) {
        if (cur(ctx)->type == TOK_STATE) {
            consume(ctx);
            ASTNode *state = node_new(ctx, NODE_STATE, cur(ctx)->line);
            if (!state) continue;
            if (cur(ctx)->type == TOK_IDENTIFIER) {
                node_copy_text(state->var_name, cur(ctx)->value);
                consume(ctx);
            }

            while (cur(ctx)->type != TOK_END && cur(ctx)->type != TOK_STATE && 
                   cur(ctx)->type != TOK_END_STATE_MACHINE && cur(ctx)->type != TOK_EOF) {
                if (cur(ctx)->type == TOK_ENTRY || cur(ctx)->type == TOK_EXIT) {
                    PlcTokenType entry_exit_type = consume(ctx)->type;
                    ASTNode *entry_exit = node_new(ctx, NODE_ACTION, cur(ctx)->line);
                    if (!entry_exit) continue;
                    strncpy(entry_exit->var_name, entry_exit_type == TOK_ENTRY ? "ENTRY" : "EXIT", 15);
                    /* Parse simple actions */
                    ASTNode *head = NULL, *tail = NULL;
                    while (cur(ctx)->type != TOK_TRANSITION && cur(ctx)->type != TOK_END && 
                           cur(ctx)->type != TOK_STATE && cur(ctx)->type != TOK_EXIT && cur(ctx)->type != TOK_ENTRY) {
                        ASTNode *act = parse_action(ctx);
                        if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
                        if (act) {
                            if (!head) { head = tail = act; }
                            else { tail->next = act; tail = act; }
                        }
                    }
                    entry_exit->next = head;
                    node_add_child(state, entry_exit);
                } else if (cur(ctx)->type == TOK_TRANSITION) {
                    consume(ctx);
                    ASTNode *trans = node_new(ctx, NODE_TRANSITION, cur(ctx)->line);
                    if (!trans) continue;
                    if (cur(ctx)->type == TOK_TO) consume(ctx);
                    if (cur(ctx)->type == TOK_IDENTIFIER) {
                        node_copy_text(trans->var_name, cur(ctx)->value);
                        consume(ctx);
                    }
                    if (cur(ctx)->type == TOK_IF) {
                        consume(ctx);
                        node_add_child(trans, parse_condition(ctx));
                    }
                    node_add_child(state, trans);
                } else {
                    ASTNode *stmt = parse_statement(ctx);
                    if (stmt) node_add_child(state, stmt);
                }
            }
            node_add_child(n, state);
            if (cur(ctx)->type == TOK_END) consume(ctx);
        } else {
            consume(ctx); /* skip or handle other things in SM */
        }
    }
    expect(ctx, TOK_END_STATE_MACHINE);
    return n;
}

/* ─── Parse Assertion ─────────────────────────────────────────────────── */
static ASTNode *parse_assertion(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_ASSERT);
    ASTNode *n = node_new(ctx, NODE_ASSERT, line);
    if (!n) return NULL;
    node_add_child(n, parse_condition(ctx));
    if (cur(ctx)->type == TOK_STRING_LITERAL) {
        node_copy_text(n->value, cur(ctx)->value);
        consume(ctx);
    }
    return n;
}

static ASTNode *parse_doc_comment(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    ASTNode *n = node_new(ctx, NODE_DOC_COMMENT, line);
    if (!n) return NULL;
    node_copy_text(n->value, cur(ctx)->value);
    consume(ctx);
    return n;
}

static ASTNode *parse_recipe_use(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    expect(ctx, TOK_USE);
    ASTNode *n = node_new(ctx, NODE_RECIPE_USE, line);
    if (!n) return NULL;

    if (cur(ctx)->type == TOK_IDENTIFIER || cur(ctx)->type == TOK_STRING_LITERAL) {
        node_copy_text(n->var_name, cur(ctx)->value);
        consume(ctx);
    } else {
        log_error(ctx, line, "Expected recipe/template name after USE, got '%s'", cur(ctx)->value);
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
    }
    return n;
}

static ASTNode *parse_const_decl(CompilerCtx *ctx, int has_keyword) {
    int line = cur(ctx)->line;
    if (has_keyword)
        expect(ctx, TOK_CONST);

    ASTNode *n = node_new(ctx, NODE_CONST_DECL, line);
    if (!n) return NULL;

    if (cur(ctx)->type != TOK_IDENTIFIER) {
        log_error(ctx, line, "Expected constant name, got '%s'", cur(ctx)->value);
        if (cur(ctx)->type != TOK_EOF) consume(ctx);
        return n;
    }

    node_copy_text(n->var_name, cur(ctx)->value);
    consume(ctx);

    if (!expect(ctx, TOK_EQ)) {
        log_error(ctx, line, "Expected '=' in constant declaration for '%s'", n->var_name);
    }
    parse_value_into(ctx, n);
    parse_optional_unit(ctx, n);
    if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
    return n;
}

/* ─── Parse variable declaration block (VAR_INPUT, VAR_OUTPUT, VAR) ─── */
static ASTNode *parse_var_block(CompilerCtx *ctx) {
    int line = cur(ctx)->line;
    PlcTokenType type = cur(ctx)->type;
    consume(ctx); /* consume VAR_INPUT, VAR_OUTPUT, or VAR */

    ASTNode *block = node_new(ctx, NODE_VAR_DECL, line);
    if (!block) return NULL;
    if (type == TOK_VAR_INPUT)       strncpy(block->value, "INPUT", 15);
    else if (type == TOK_VAR_OUTPUT) strncpy(block->value, "OUTPUT", 15);
    else                             strncpy(block->value, "MEMORY", 15);

    while (cur(ctx)->type != TOK_END_VAR && cur(ctx)->type != TOK_EOF) {
        if (cur(ctx)->type == TOK_IDENTIFIER) {
            ASTNode *decl = node_new(ctx, NODE_VAR_DECL, cur(ctx)->line);
            if (!decl) {
                consume(ctx);
                continue;
            }
            node_copy_text(decl->var_name, cur(ctx)->value);
            consume(ctx);

            if (cur(ctx)->type == TOK_COLON) {
                consume(ctx);
                /* Type name (BOOL, INT, REAL) */
                if (cur(ctx)->type == TOK_IDENTIFIER) {
                    node_copy_text(decl->value, cur(ctx)->value);
                    consume(ctx);
                } else {
                    log_error(ctx, decl->line, "Expected type name after ':', got '%s'", cur(ctx)->value);
                    if (cur(ctx)->type != TOK_EOF) consume(ctx);
                }
            } else if (cur(ctx)->type == TOK_IN) {
                node_copy_text(decl->value, "REAL");
            }
            parse_optional_unit(ctx, decl);
            parse_annotations(ctx, decl);
            node_add_child(block, decl);
            if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
        } else {
            consume(ctx); /* skip unknown */
        }
    }
    expect(ctx, TOK_END_VAR);
    return block;
}

/* ─── Parse statement ───────────────────────────────────────────────────── */
static ASTNode *parse_statement(CompilerCtx *ctx) {
    if (cur(ctx)->type == TOK_DOC_COMMENT) return parse_doc_comment(ctx);
    if (cur(ctx)->type == TOK_CONST) return parse_const_decl(ctx, 1);
    if (cur(ctx)->type == TOK_USE) return parse_recipe_use(ctx);
    if (cur(ctx)->type == TOK_IF)    return parse_if(ctx);
    if (cur(ctx)->type == TOK_WHILE) return parse_while(ctx);
    if (cur(ctx)->type == TOK_CASE)  return parse_case(ctx);
    if (cur(ctx)->type == TOK_STRUCT) return parse_struct(ctx);
    if (cur(ctx)->type == TOK_FUNCTION_BLOCK) return parse_function_block(ctx);
    if (cur(ctx)->type == TOK_STATE_MACHINE) return parse_state_machine(ctx);
    if (cur(ctx)->type == TOK_ASSERT) return parse_assertion(ctx);
    
    if (cur(ctx)->type == TOK_VAR || cur(ctx)->type == TOK_VAR_INPUT || cur(ctx)->type == TOK_VAR_OUTPUT)
        return parse_var_block(ctx);

    /* Bare assignment at program level */
    if (cur(ctx)->type == TOK_IDENTIFIER &&
        peek_tok(ctx, 1)->type == TOK_EQ) {
        if (is_const_identifier(cur(ctx)->value) &&
            (peek_tok(ctx, 2)->type == TOK_NUMBER ||
             peek_tok(ctx, 2)->type == TOK_STRING_LITERAL ||
             peek_tok(ctx, 2)->type == TOK_IDENTIFIER)) {
            return parse_const_decl(ctx, 0);
        }
        ASTNode *act = parse_action(ctx);
        if (cur(ctx)->type == TOK_SEMICOLON) consume(ctx);
        return act;
    }

    log_error(ctx, cur(ctx)->line, "Unexpected token '%s' ('%s') — expected IF, WHILE, CASE, STRUCT, FB, SM, or assignment",
              token_type_name(cur(ctx)->type), cur(ctx)->value);
    consume(ctx);
    return NULL;
}

/* ─── Top-level parse ───────────────────────────────────────────────────── */
ASTNode *parser_parse(CompilerCtx *ctx) {
    log_info(ctx, "Parser: building AST");
    ASTNode *root = node_new(ctx, NODE_PROGRAM, 0);
    if (!root) return NULL;

    while (cur(ctx)->type != TOK_EOF) {
        ASTNode *stmt = parse_statement(ctx);
        if (stmt) node_add_child(root, stmt);
    }

    log_info(ctx, "Parser: AST built (%d top-level statements, %d nodes allocated)",
             root->child_count, ctx->nodes_allocated);
    return root;
}

/* ─── AST pretty-printer ─────────────────────────────────────────────────── */
static const char *cmp_op_str(CmpOp op) {
    switch (op) {
        case CMP_EQ:  return "=";
        case CMP_NEQ: return "!=";
        case CMP_GTE: return ">=";
        case CMP_LTE: return "<=";
        case CMP_GT:  return ">";
        case CMP_LT:  return "<";
        default:      return "?";
    }
}

static const char *timer_unit_str(TimerUnit u) {
    switch (u) {
        case TIMER_SECONDS:      return "sec";
        case TIMER_MINUTES:      return "min";
        case TIMER_MILLISECONDS: return "ms";
        default:                 return "?";
    }
}

void ast_print(ASTNode *node, int depth) {
    if (!node) return;
    char indent[128] = {0};
    for (int i = 0; i < depth && i < 60; i++) indent[i] = ' ';

    switch (node->type) {
        case NODE_PROGRAM:
            printf("%s[PROGRAM] (%d statements)\n", indent, node->child_count);
            break;
        case NODE_IF:
            printf("%s[IF] line=%d", indent, node->line);
            if (node->has_timer)
                printf(" | Timer: %.2f %s", node->timer_value, timer_unit_str(node->timer_unit));
            printf("\n");
            break;
        case NODE_COMPARISON:
            printf("%s[CONDITION] %s %s %s%s\n", indent,
                   node->var_name, cmp_op_str(node->cmp_op), node->value,
                   node->is_numeric ? " (numeric)" : "");
            break;
        case NODE_AND: printf("%s[AND]\n", indent); break;
        case NODE_OR:  printf("%s[OR]\n",  indent); break;
        case NODE_NOT: printf("%s[NOT]\n", indent); break;
        case NODE_ACTION:
            if (node->var_name[0])
                printf("%s[ACTION] %s = %s\n", indent, node->var_name, node->value);
            else
                printf("%s[ACTION-LIST]\n", indent);
            break;
        case NODE_DOC_COMMENT:
            printf("%s[DOC] %s\n", indent, node->value);
            break;
        case NODE_CONST_DECL:
            printf("%s[CONST] %s = %s%s%s\n", indent, node->var_name, node->value,
                   node->unit[0] ? " IN " : "", node->unit);
            break;
        case NODE_RECIPE_USE:
            printf("%s[USE] %s\n", indent, node->var_name);
            break;
        case NODE_WHILE:
            printf("%s[WHILE] line=%d\n", indent, node->line);
            break;
        case NODE_CASE:
            printf("%s[CASE] var=%s line=%d\n", indent, node->var_name, node->line);
            break;
        case NODE_CASE_BRANCH:
            printf("%s[CASE_BRANCH] value=%s\n", indent, node->value);
            break;
        case NODE_REPEAT:
            printf("%s[REPEAT] line=%d\n", indent, node->line);
            break;
        default:
            printf("%s[NODE type=%d]\n", indent, node->type);
    }

    /* Children */
    const char *labels[] = { "Condition", "Then", "Else", "Child" };
    for (int i = 0; i < node->child_count; i++) {
        const char *lbl = (node->type == NODE_IF && i < 3) ? labels[i] : labels[3];
        printf("%s  ├─ %s:\n", indent, lbl);
        ast_print(node->children[i], depth + 5);
    }

    /* Action linked list */
    if (node->type == NODE_ACTION && node->next) {
        printf("%s  ├─ Next Action:\n", indent);
        ast_print(node->next, depth + 5);
    }
}
