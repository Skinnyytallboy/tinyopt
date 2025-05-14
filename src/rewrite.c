#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rewrite.h"

static int fold_pred(const Pred *p) {
    if (p->lhs_is_col || p->rhs_is_col) return -1;
    int c = value_compare(&p->lhs_lit, &p->rhs_lit);
    switch (p->op) {
        case OP_EQ: return c == 0;
        case OP_NE: return c != 0;
        case OP_LT: return c < 0;
        case OP_LE: return c <= 0;
        case OP_GT: return c > 0;
        case OP_GE: return c >= 0;
    }
    return -1;
}

static PlanNode *cf_rec(PlanNode *n) {
    if (!n) return n;
    n->left = cf_rec(n->left);
    n->right = cf_rec(n->right);

    if (n->kind == NODE_FILTER) {
        Pred kept[MAX_PREDS];
        int nkept = 0;
        for (int i = 0; i < n->n_preds; i++) {
            int r = fold_pred(&n->preds[i]);
            if (r == 1) {
                value_free(&n->preds[i].lhs_lit);
                value_free(&n->preds[i].rhs_lit);
                continue;
            }
            if (r == 0) {
                n->always_false = 1;
                value_free(&n->preds[i].lhs_lit);
                value_free(&n->preds[i].rhs_lit);
                continue;
            }
            kept[nkept++] = n->preds[i];
        }
        for (int i = 0; i < nkept; i++) n->preds[i] = kept[i];
        n->n_preds = n->always_false ? 0 : nkept;
    }
    return n;
}

PlanNode *rule_constant_fold(PlanNode *plan) {
    return cf_rec(plan);
}

typedef struct { Pred items[MAX_PREDS]; int n; } PredList;

static void pl_remove(PredList *l, int idx) {
    l->items[idx] = l->items[--l->n];
}

static int col_belongs_to(const ColRef *c, const char *table) {
    return strcmp(c->table, table) == 0;
}

static int pred_is_single_table(const Pred *p, const char *table) {
    if (p->lhs_is_col && !col_belongs_to(&p->lhs_col, table)) return 0;
    if (p->rhs_is_col && !col_belongs_to(&p->rhs_col, table)) return 0;
    return 1;
}

static int table_in_set(const char *t, char set[][NAME_LEN], int n) {
    for (int i = 0; i < n; i++) if (strcmp(set[i], t) == 0) return 1;
    return 0;
}

static int pred_spans(const Pred *p, char left[][NAME_LEN], int nl, char right[][NAME_LEN], int nr) {
    const char *lt = p->lhs_is_col ? p->lhs_col.table : NULL;
    const char *rt = p->rhs_is_col ? p->rhs_col.table : NULL;
    if (!lt || !rt) return 0;
    if (table_in_set(lt, left, nl) && table_in_set(rt, right, nr)) return 1;
    if (table_in_set(rt, left, nl) && table_in_set(lt, right, nr)) return 1;
    return 0;
}

static PlanNode *distribute(PlanNode *node, PredList *pending) {
    if (node->kind == NODE_SCAN) {
        PredList mine = {0};
        for (int i = 0; i < pending->n; ) {
            if (pred_is_single_table(&pending->items[i], node->table)) {
                mine.items[mine.n++] = pending->items[i];
                pl_remove(pending, i);
            } else i++;
        }
        if (mine.n == 0) return node;
        PlanNode *f = plan_new(NODE_FILTER);
        f->left = node;
        f->n_preds = mine.n;
        for (int i = 0; i < mine.n; i++) f->preds[i] = mine.items[i];
        return f;
    }

    node->left = distribute(node->left, pending);
    node->right = distribute(node->right, pending);

    char lt[MAX_TABLES][NAME_LEN], rt[MAX_TABLES][NAME_LEN];
    int nlt = plan_collect_tables(node->left, lt, MAX_TABLES);
    int nrt = plan_collect_tables(node->right, rt, MAX_TABLES);

    for (int i = 0; i < pending->n; ) {
        if (pred_spans(&pending->items[i], lt, nlt, rt, nrt)) {
            if (node->n_join_conds < MAX_PREDS) {
                node->join_conds[node->n_join_conds++] = pending->items[i];
                node->kind = NODE_JOIN;
            }
            pl_remove(pending, i);
        } else i++;
    }
    return node;
}

static PlanNode *pd_rec(PlanNode *n) {
    if (!n) return n;
    if (n->kind == NODE_FILTER && n->left &&
        (n->left->kind == NODE_CROSS || n->left->kind == NODE_JOIN)) {
        PredList pending = {0};
        pending.n = n->n_preds;
        for (int i = 0; i < n->n_preds; i++) pending.items[i] = n->preds[i];

        PlanNode *joined = distribute(n->left, &pending);

        PlanNode *result;
        if (pending.n > 0) {
            n->left = joined;
            n->n_preds = pending.n;
            for (int i = 0; i < pending.n; i++) n->preds[i] = pending.items[i];
            result = n;
        } else {
            free(n);
            result = joined;
        }
        return result;
    }
    n->left = pd_rec(n->left);
    n->right = pd_rec(n->right);
    return n;
}

PlanNode *rule_predicate_pushdown(PlanNode *plan) {
    return pd_rec(plan);
}

static PlanNode *distribute_joins_only(PlanNode *node, PredList *pending) {
    if (node->kind == NODE_SCAN) return node;

    node->left = distribute_joins_only(node->left, pending);
    node->right = distribute_joins_only(node->right, pending);

    char lt[MAX_TABLES][NAME_LEN], rt[MAX_TABLES][NAME_LEN];
    int nlt = plan_collect_tables(node->left, lt, MAX_TABLES);
    int nrt = plan_collect_tables(node->right, rt, MAX_TABLES);

    for (int i = 0; i < pending->n; ) {
        if (pred_spans(&pending->items[i], lt, nlt, rt, nrt)) {
            if (node->n_join_conds < MAX_PREDS) {
                node->join_conds[node->n_join_conds++] = pending->items[i];
                node->kind = NODE_JOIN;
            }
            pl_remove(pending, i);
        } else i++;
    }
    return node;
}

PlanNode *rule_attach_join_conditions(PlanNode *plan) {
    if (!plan) return plan;
    if (plan->kind == NODE_FILTER && plan->left &&
        (plan->left->kind == NODE_CROSS || plan->left->kind == NODE_JOIN)) {
        PredList pending = {0};
        pending.n = plan->n_preds;
        for (int i = 0; i < plan->n_preds; i++) pending.items[i] = plan->preds[i];

        plan->left = distribute_joins_only(plan->left, &pending);
        plan->n_preds = pending.n;
        for (int i = 0; i < pending.n; i++) plan->preds[i] = pending.items[i];
        return plan;
    }
    plan->left = rule_attach_join_conditions(plan->left);
    plan->right = rule_attach_join_conditions(plan->right);
    return plan;
}

static void colset_add(ColRef *set, int *n, int max, ColRef c) {
    if (strcmp(c.col, "*") == 0) return;
    for (int i = 0; i < *n; i++)
        if (strcmp(set[i].table, c.table) == 0 && strcmp(set[i].col, c.col) == 0) return;
    if (*n < max) set[(*n)++] = c;
}

#define NEED_ALL (-1)

static PlanNode *pp_rec(PlanNode *n, ColRef *needed, int nneeded) {
    if (!n) return n;

    switch (n->kind) {
    case NODE_PROJECT: {
        if (n->is_star) {
            n->left = pp_rec(n->left, NULL, NEED_ALL);
        } else {
            ColRef want[64]; int nw = 0;
            for (int i = 0; i < n->n_exprs; i++) expr_collect_columns(n->exprs[i], want, &nw, 64);
            n->left = pp_rec(n->left, want, nw);
        }
        return n;
    }
    case NODE_GROUPBY: {
        ColRef want[64]; int nw = 0;
        colset_add(want, &nw, 64, n->group_col);
        expr_collect_columns(n->agg_expr, want, &nw, 64);
        n->left = pp_rec(n->left, want, nw);
        return n;
    }
    case NODE_LIMIT: {
        n->left = pp_rec(n->left, needed, nneeded);
        return n;
    }
    case NODE_FILTER: {
        ColRef want[64]; int nw = 0;
        if (nneeded != NEED_ALL)
            for (int i = 0; i < nneeded; i++) colset_add(want, &nw, 64, needed[i]);
        for (int i = 0; i < n->n_preds; i++) {
            if (n->preds[i].lhs_is_col) colset_add(want, &nw, 64, n->preds[i].lhs_col);
            if (n->preds[i].rhs_is_col) colset_add(want, &nw, 64, n->preds[i].rhs_col);
        }
        n->left = pp_rec(n->left, want, nneeded == NEED_ALL ? NEED_ALL : nw);
        return n;
    }
    case NODE_JOIN:
    case NODE_CROSS: {
        ColRef want[64]; int nw = 0;
        if (nneeded != NEED_ALL)
            for (int i = 0; i < nneeded; i++) colset_add(want, &nw, 64, needed[i]);
        for (int i = 0; i < n->n_join_conds; i++) {
            if (n->join_conds[i].lhs_is_col) colset_add(want, &nw, 64, n->join_conds[i].lhs_col);
            if (n->join_conds[i].rhs_is_col) colset_add(want, &nw, 64, n->join_conds[i].rhs_col);
        }
        char lt[MAX_TABLES][NAME_LEN], rt[MAX_TABLES][NAME_LEN];
        int nlt = plan_collect_tables(n->left, lt, MAX_TABLES);
        int nrt = plan_collect_tables(n->right, rt, MAX_TABLES);

        ColRef lwant[64], rwant[64]; int nlw = 0, nrw = 0;
        int all = (nneeded == NEED_ALL);
        for (int i = 0; i < nw; i++) {
            if (table_in_set(want[i].table, lt, nlt)) colset_add(lwant, &nlw, 64, want[i]);
            if (table_in_set(want[i].table, rt, nrt)) colset_add(rwant, &nrw, 64, want[i]);
        }
        n->left = pp_rec(n->left, lwant, all ? NEED_ALL : nlw);
        n->right = pp_rec(n->right, rwant, all ? NEED_ALL : nrw);
        return n;
    }
    case NODE_SCAN: {
        if (nneeded == NEED_ALL || nneeded <= 0) return n;
        PlanNode *proj = plan_new(NODE_PROJECT);
        proj->left = n;
        proj->n_exprs = 0;
        for (int i = 0; i < nneeded && proj->n_exprs < MAX_EXPRS; i++) {
            if (!col_belongs_to(&needed[i], n->table)) continue;
            Expr *e = calloc(1, sizeof(Expr));
            e->kind = EXPR_COL;
            e->col = needed[i];
            proj->exprs[proj->n_exprs++] = e;
        }
        if (proj->n_exprs == 0) { free(proj); return n; }
        return proj;
    }
    }
    return n;
}

PlanNode *rule_projection_pushdown(PlanNode *plan) {
    return pp_rec(plan, NULL, NEED_ALL);
}

/* ============================== rule 4: join input swap ============================== */

static void swap_children(PlanNode *n) {
    PlanNode *tmp = n->left;
    n->left = n->right;
    n->right = tmp;
    for (int i = 0; i < n->n_join_conds; i++) {
        Pred *p = &n->join_conds[i];
        if (p->lhs_is_col && p->rhs_is_col) {
            ColRef t = p->lhs_col; p->lhs_col = p->rhs_col; p->rhs_col = t;
        }
    }
}

void rule_join_swap(PlanNode *n) {
    if (!n) return;
    rule_join_swap(n->left);
    rule_join_swap(n->right);
    if ((n->kind == NODE_JOIN || n->kind == NODE_CROSS) &&
        n->left && n->right && n->left->est_cardinality >= 0 && n->right->est_cardinality >= 0) {
        if (n->right->est_cardinality < n->left->est_cardinality) {
            swap_children(n);
        }
    }
}

static int plan_signature(const PlanNode *n, char *buf, int len) {
    if (!n) return snprintf(buf, len, ".");
    int off = snprintf(buf, len, "%d:%s:%d:%d:%d|", n->kind, n->table, n->n_preds,
                        n->n_join_conds, n->n_exprs);
    if (off < len) off += plan_signature(n->left, buf + off, len - off);
    if (off < len) off += plan_signature(n->right, buf + off, len - off);
    return off;
}

PlanNode *rewrite_to_fixpoint(PlanNode *plan) {
    char before[8192], after[8192];
    for (int iter = 0; iter < 8; iter++) {
        plan_signature(plan, before, sizeof(before));
        plan = rule_constant_fold(plan);
        plan = rule_predicate_pushdown(plan);
        plan_signature(plan, after, sizeof(after));
        if (strcmp(before, after) == 0) break;
    }
    plan = rule_projection_pushdown(plan);
    return plan;
}
