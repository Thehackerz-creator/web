/**
 * logic_graph.c - Graphviz DOT export for logic, execution, and safety paths.
 */

#include "plc_compiler.h"

static void dot_escape(FILE *f, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        if (*p == '\n' || *p == '\r' || *p == '\t') fputc(' ', f);
        else if (*p >= 0x20) fputc(*p, f);
    }
}

static const char *node_type_label(NodeType type) {
    switch (type) {
        case NODE_IF: return "IF";
        case NODE_WHILE: return "WHILE";
        case NODE_CASE: return "CASE";
        case NODE_CASE_BRANCH: return "CASE_BRANCH";
        case NODE_COMPARISON: return "CMP";
        case NODE_AND: return "AND";
        case NODE_OR: return "OR";
        case NODE_NOT: return "NOT";
        case NODE_ACTION: return "ACTION";
        case NODE_STATE_MACHINE: return "STATE_MACHINE";
        case NODE_STATE: return "STATE";
        case NODE_TRANSITION: return "TRANSITION";
        case NODE_ASSERT: return "ASSERT";
        case NODE_VAR_DECL: return "VAR";
        case NODE_CONST_DECL: return "CONST";
        default: return "NODE";
    }
}

static const char *cmp_label(CmpOp op) {
    switch (op) {
        case CMP_EQ: return "=";
        case CMP_NEQ: return "!=";
        case CMP_GTE: return ">=";
        case CMP_LTE: return "<=";
        case CMP_GT: return ">";
        case CMP_LT: return "<";
        default: return "?";
    }
}

static int node_id(ASTNode *node) {
    return node ? (int)(((unsigned long)(void *)node) & 0x7fffffffUL) : 0;
}

static void emit_symbol_id(FILE *f, const char *name) {
    fprintf(f, "\"sym:");
    dot_escape(f, name);
    fprintf(f, "\"");
}

static void emit_ast_node(FILE *f, ASTNode *node) {
    if (!node) return;
    fprintf(f, "  n%d [label=\"%s", node_id(node), node_type_label(node->type));
    if (node->var_name[0]) {
        fprintf(f, "\\n");
        dot_escape(f, node->var_name);
    }
    if (node->type == NODE_COMPARISON) {
        fprintf(f, " ");
        dot_escape(f, cmp_label(node->cmp_op));
        fprintf(f, " ");
        dot_escape(f, node->value);
    } else if (node->value[0] && node->type != NODE_VAR_DECL) {
        fprintf(f, "\\n");
        dot_escape(f, node->value);
    }
    fprintf(f, "\", shape=%s];\n",
            node->type == NODE_ACTION ? "box" :
            node->type == NODE_COMPARISON ? "diamond" : "ellipse");
}

static void emit_ast_recursive(FILE *f, ASTNode *node) {
    if (!node) return;
    emit_ast_node(f, node);
    for (int i = 0; i < node->child_count; i++) {
        if (!node->children[i]) continue;
        emit_ast_recursive(f, node->children[i]);
        fprintf(f, "  n%d -> n%d [label=\"child\"];\n",
                node_id(node), node_id(node->children[i]));
    }
    if (node->next) {
        emit_ast_recursive(f, node->next);
        fprintf(f, "  n%d -> n%d [label=\"next\", style=dashed];\n",
                node_id(node), node_id(node->next));
    }
}

static void emit_condition_reads(FILE *f, ASTNode *cond, ASTNode *owner) {
    if (!cond || !owner) return;
    if (cond->type == NODE_COMPARISON && cond->var_name[0]) {
        fprintf(f, "  ");
        emit_symbol_id(f, cond->var_name);
        fprintf(f, " -> n%d [label=\"guards\", color=\"#2563eb\"];\n", node_id(owner));
    }
    for (int i = 0; i < cond->child_count; i++)
        emit_condition_reads(f, cond->children[i], owner);
}

static void emit_action_writes(FILE *f, ASTNode *actions, ASTNode *owner) {
    for (ASTNode *act = actions; act; act = act->next) {
        if (act->type != NODE_ACTION || !act->var_name[0]) continue;
        fprintf(f, "  n%d -> ", node_id(owner));
        emit_symbol_id(f, act->var_name);
        fprintf(f, " [label=\"writes ");
        dot_escape(f, act->value[0] ? act->value : "value");
        fprintf(f, "\", color=\"#16a34a\"];\n");
    }
}

static void emit_flow_edges(FILE *f, ASTNode *node) {
    if (!node) return;
    if (node->type == NODE_IF && node->child_count > 1) {
        emit_condition_reads(f, node->children[0], node);
        if (node->children[1]) emit_action_writes(f, node->children[1]->next, node);
        if (node->child_count > 2 && node->children[2])
            emit_action_writes(f, node->children[2]->next, node);
    } else if (node->type == NODE_STATE && node->var_name[0]) {
        for (int i = 0; i < node->child_count; i++) {
            ASTNode *child = node->children[i];
            if (child && child->type == NODE_TRANSITION && child->var_name[0]) {
                fprintf(f, "  n%d -> n%d [label=\"transition to ", node_id(node), node_id(child));
                dot_escape(f, child->var_name);
                fprintf(f, "\", color=\"#7c3aed\"];\n");
            }
        }
    }
    for (int i = 0; i < node->child_count; i++)
        emit_flow_edges(f, node->children[i]);
    if (node->next)
        emit_flow_edges(f, node->next);
}

static void emit_symbols(FILE *f, CompilerCtx *ctx) {
    fprintf(f, "  subgraph cluster_symbols {\n");
    fprintf(f, "    label=\"Variables\";\n");
    fprintf(f, "    color=\"#d4d4d8\";\n");
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        fprintf(f, "    ");
        emit_symbol_id(f, s->name);
        fprintf(f, " [label=\"");
        dot_escape(f, s->name);
        fprintf(f, "\\n%s%s%s\", shape=note, style=filled, fillcolor=\"%s\"];\n",
                s->direction == IO_INPUT ? "INPUT" :
                s->direction == IO_OUTPUT ? "OUTPUT" :
                s->direction == IO_TIMER_VAR ? "TIMER" : "MEMORY",
                s->safety_estop ? " @ESTOP" : "",
                s->safety_critical || s->safety_sil_level ? " @SAFE" : "",
                s->safety_estop ? "#fee2e2" :
                (s->safety_critical || s->safety_sil_level) ? "#fef3c7" : "#f8fafc");
    }
    fprintf(f, "  }\n");
}

static void emit_safety_paths(FILE *f, CompilerCtx *ctx) {
    if (!ctx->run_safety) return;
    fprintf(f, "  subgraph cluster_safety {\n");
    fprintf(f, "    label=\"Safety Paths\";\n");
    fprintf(f, "    color=\"#dc2626\";\n");
    for (int i = 0; i < ctx->safety_result.output_count; i++) {
        OutputTracker *ot = &ctx->safety_result.outputs[i];
        Symbol *s = sym_lookup(ctx, ot->name);
        if (!s || !(s->safety_critical || s->safety_sil_level)) continue;
        fprintf(f, "    safety_%d [label=\"", i);
        dot_escape(f, ot->name);
        fprintf(f, "\\n%s\", shape=octagon, style=filled, fillcolor=\"%s\"];\n",
                ot->covered_by_estop ? "E-stop covered" : "needs E-stop",
                ot->covered_by_estop ? "#dcfce7" : "#fee2e2");
        fprintf(f, "    safety_%d -> ", i);
        emit_symbol_id(f, ot->name);
        fprintf(f, " [style=dotted, color=\"#dc2626\"];\n");
    }
    fprintf(f, "  }\n");
}

int graph_write_dot(CompilerCtx *ctx, const char *path) {
    FILE *f;
    if (!ctx || !ctx->ast_root || !path) return 0;
    f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f, "digraph AtlasLogic {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  graph [fontname=\"Inter\", label=\"ATLAS Logic Graph\", labelloc=t];\n");
    fprintf(f, "  node [fontname=\"Inter\", fontsize=10];\n");
    fprintf(f, "  edge [fontname=\"Inter\", fontsize=9];\n");

    emit_symbols(f, ctx);
    fprintf(f, "  subgraph cluster_execution {\n");
    fprintf(f, "    label=\"Execution Flow\";\n");
    fprintf(f, "    color=\"#93c5fd\";\n");
    emit_ast_recursive(f, ctx->ast_root);
    fprintf(f, "  }\n");
    emit_flow_edges(f, ctx->ast_root);
    emit_safety_paths(f, ctx);

    fprintf(f, "}\n");
    fclose(f);
    return 1;
}
