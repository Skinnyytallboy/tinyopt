#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plan.h"

PlanNode *plan_new(NodeKind kind) {
    PlanNode *n = calloc(1, sizeof(PlanNode));
    n->kind = kind;
    n->est_cardinality = -1;
    n->est_cost = -1;
    n->actual_cardinality = -1;
    return n;
}

PlanNode *plan_scan(const char *table) {
    PlanNode *n = plan_new(NODE_SCAN);
    snprintf(n->table, NAME_LEN, "%s", table);
    return n;
}

Expr *expr_clone(const Expr *e) {
    if (!e) return NULL;
    Expr *c = calloc(1, sizeof(Expr));
    *c = *e;
    c->lit = value_copy(&e->lit);
    c->left = expr_clone(e->left);
    c->right = expr_clone(e->right);
    return c;
}

void expr_free(Expr *e) {
    if (!e) return;
    value_free(&e->lit);
    expr_free(e->left);
    expr_free(e->right);
    free(e);
}

static Pred pred_clone(const Pred *p) {
    Pred c = *p;
    c.lhs_lit = value_copy(&p->lhs_lit);
    c.rhs_lit = value_copy(&p->rhs_lit);
    return c;
}

PlanNode *plan_clone(const PlanNode *n) {
    if (!n) return NULL;
    PlanNode *c = calloc(1, sizeof(PlanNode));
    *c = *n;
    for (int i = 0; i < n->n_preds; i++) c->preds[i] = pred_clone(&n->preds[i]);
    for (int i = 0; i < n->n_join_conds; i++) c->join_conds[i] = pred_clone(&n->join_conds[i]);
    for (int i = 0; i < n->n_exprs; i++) c->exprs[i] = expr_clone(n->exprs[i]);
    c->agg_expr = expr_clone(n->agg_expr);
    c->left = plan_clone(n->left);
    c->right = plan_clone(n->right);
    return c;
}

void plan_free(PlanNode *n) {
    if (!n) return;
    for (int i = 0; i < n->n_preds; i++) {
        value_free(&n->preds[i].lhs_lit);
        value_free(&n->preds[i].rhs_lit);
    }
    for (int i = 0; i < n->n_join_conds; i++) {
        value_free(&n->join_conds[i].lhs_lit);
        value_free(&n->join_conds[i].rhs_lit);
    }
    for (int i = 0; i < n->n_exprs; i++) expr_free(n->exprs[i]);
    expr_free(n->agg_expr);
    plan_free(n->left);
    plan_free(n->right);
    free(n);
}

int expr_references_table(const Expr *e, const char *t) {
    if (!e) return 0;
    if ((e->kind == EXPR_COL || e->kind == EXPR_AGG) && strcmp(e->col.table, t) == 0) return 1;
    if (expr_references_table(e->left, t)) return 1;
    if (expr_references_table(e->right, t)) return 1;
    return 0;
}

void expr_collect_columns(const Expr *e, ColRef *out, int *n, int max) {
    if (!e || *n >= max) return;
    if (e->kind == EXPR_COL || e->kind == EXPR_AGG) {
        out[(*n)++] = e->col;
    }
    expr_collect_columns(e->left, out, n, max);
    expr_collect_columns(e->right, out, n, max);
}

static int preds_reference_table(const Pred *preds, int n, const char *t) {
    for (int i = 0; i < n; i++) {
        if (preds[i].lhs_is_col && strcmp(preds[i].lhs_col.table, t) == 0) return 1;
        if (preds[i].rhs_is_col && strcmp(preds[i].rhs_col.table, t) == 0) return 1;
    }
    return 0;
}

int plan_references_table(const PlanNode *n, const char *t) {
    if (!n) return 0;
    if (n->kind == NODE_SCAN) return strcmp(n->table, t) == 0;
    if (preds_reference_table(n->preds, n->n_preds, t)) return 1;
    if (preds_reference_table(n->join_conds, n->n_join_conds, t)) return 1;
    for (int i = 0; i < n->n_exprs; i++)
        if (expr_references_table(n->exprs[i], t)) return 1;
    if (expr_references_table(n->agg_expr, t)) return 1;
    if (n->kind == NODE_GROUPBY && strcmp(n->group_col.table, t) == 0) return 1;
    return plan_references_table(n->left, t) || plan_references_table(n->right, t);
}

static int seen(char out[][NAME_LEN], int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(out[i], name) == 0) return 1;
    return 0;
}

static void collect_rec(const PlanNode *n, char out[][NAME_LEN], int *count, int max) {
    if (!n || *count >= max) return;
    if (n->kind == NODE_SCAN) {
        if (!seen(out, *count, n->table))
            snprintf(out[(*count)++], NAME_LEN, "%s", n->table);
        return;
    }
    collect_rec(n->left, out, count, max);
    collect_rec(n->right, out, count, max);
}

int plan_collect_tables(const PlanNode *n, char out[][NAME_LEN], int max) {
    int count = 0;
    collect_rec(n, out, &count, max);
    return count;
}

const char *cmpop_str(CmpOp op) {
    switch (op) {
        case OP_EQ: return "=";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_NE: return "!=";
    }
    return "?";
}

const char *plan_kind_str(NodeKind k) {
    switch (k) {
        case NODE_SCAN: return "SeqScan";
        case NODE_FILTER: return "Filter";
        case NODE_JOIN: return "HashJoin";
        case NODE_CROSS: return "CrossProduct";
        case NODE_PROJECT: return "Project";
        case NODE_GROUPBY: return "GroupBy";
        case NODE_LIMIT: return "Limit";
    }
    return "?";
}

static void pred_str(const Pred *p, char *buf, int len) {
    char lhsb[128], rhsb[128];
    if (p->lhs_is_col) snprintf(lhsb, sizeof(lhsb), "%s.%s", p->lhs_col.table, p->lhs_col.col);
    else value_print(&p->lhs_lit, lhsb, sizeof(lhsb));
    if (p->rhs_is_col) snprintf(rhsb, sizeof(rhsb), "%s.%s", p->rhs_col.table, p->rhs_col.col);
    else value_print(&p->rhs_lit, rhsb, sizeof(rhsb));
    snprintf(buf, len, "%s %s %s", lhsb, cmpop_str(p->op), rhsb);
}

void plan_print(const PlanNode *n, int indent, int with_cost) {
    if (!n) return;
    char pad[128];
    int pl = indent * 2;
    if (pl > 120) pl = 120;
    memset(pad, ' ', pl);
    pad[pl] = 0;

    char extra[512] = "";
    if (n->kind == NODE_SCAN) {
        snprintf(extra, sizeof(extra), "(%s)", n->table);
    } else if (n->kind == NODE_FILTER) {
        char preds[400] = "";
        for (int i = 0; i < n->n_preds; i++) {
            char pb[160];
            pred_str(&n->preds[i], pb, sizeof(pb));
            strncat(preds, pb, sizeof(preds) - strlen(preds) - 1);
            if (i + 1 < n->n_preds) strncat(preds, " AND ", sizeof(preds) - strlen(preds) - 1);
        }
        snprintf(extra, sizeof(extra), "(%s)", preds);
    } else if (n->kind == NODE_JOIN) {
        char preds[300] = "";
        for (int i = 0; i < n->n_join_conds; i++) {
            char pb[160];
            pred_str(&n->join_conds[i], pb, sizeof(pb));
            strncat(preds, pb, sizeof(preds) - strlen(preds) - 1);
            if (i + 1 < n->n_join_conds) strncat(preds, " AND ", sizeof(preds) - strlen(preds) - 1);
        }
        snprintf(extra, sizeof(extra), "(%s)", preds);
    } else if (n->kind == NODE_LIMIT) {
        snprintf(extra, sizeof(extra), "(%ld)", n->limit_n);
    } else if (n->kind == NODE_GROUPBY) {
        snprintf(extra, sizeof(extra), "(%s.%s)", n->group_col.table, n->group_col.col);
    }

    if (with_cost && n->est_cardinality >= 0) {
        printf("%s%s%s  [est %.0f rows, cost %.0f]\n", pad, plan_kind_str(n->kind), extra,
               n->est_cardinality, n->est_cost);
    } else {
        printf("%s%s%s\n", pad, plan_kind_str(n->kind), extra);
    }

    plan_print(n->left, indent + 1, with_cost);
    plan_print(n->right, indent + 1, with_cost);
}
