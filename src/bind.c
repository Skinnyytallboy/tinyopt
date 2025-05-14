#include <stdio.h>
#include <string.h>
#include "bind.h"

static int resolve_one(ColRef *c, char tables[][NAME_LEN], int ntables, Catalog *cat,
                        char *errbuf, int errbuf_len) {
    if (c->table[0] != 0) return 0;
    if (strcmp(c->col, "*") == 0) return 0;

    int matches = 0;
    const char *found = NULL;
    for (int i = 0; i < ntables; i++) {
        TableStats *ts = catalog_find_table(cat, tables[i]);
        if (!ts) continue;
        if (catalog_find_column(ts, c->col)) {
            matches++;
            found = tables[i];
        }
    }
    if (matches == 0) {
        snprintf(errbuf, errbuf_len, "unknown column '%s'", c->col);
        return -1;
    }
    if (matches > 1) {
        snprintf(errbuf, errbuf_len, "ambiguous column '%s', qualify with table name", c->col);
        return -1;
    }
    snprintf(c->table, NAME_LEN, "%s", found);
    return 0;
}

static int resolve_expr(Expr *e, char tables[][NAME_LEN], int ntables, Catalog *cat,
                         char *errbuf, int errbuf_len) {
    if (!e) return 0;
    if ((e->kind == EXPR_COL || e->kind == EXPR_AGG) && strcmp(e->col.col, "*") != 0) {
        if (resolve_one(&e->col, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    }
    if (resolve_expr(e->left, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    if (resolve_expr(e->right, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    return 0;
}

static int resolve_preds(Pred *preds, int n, char tables[][NAME_LEN], int ntables, Catalog *cat,
                          char *errbuf, int errbuf_len) {
    for (int i = 0; i < n; i++) {
        if (preds[i].lhs_is_col &&
            resolve_one(&preds[i].lhs_col, tables, ntables, cat, errbuf, errbuf_len) != 0)
            return -1;
        if (preds[i].rhs_is_col &&
            resolve_one(&preds[i].rhs_col, tables, ntables, cat, errbuf, errbuf_len) != 0)
            return -1;
    }
    return 0;
}

static int bind_rec(PlanNode *n, char tables[][NAME_LEN], int ntables, Catalog *cat,
                     char *errbuf, int errbuf_len) {
    if (!n) return 0;
    if (resolve_preds(n->preds, n->n_preds, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    if (resolve_preds(n->join_conds, n->n_join_conds, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    for (int i = 0; i < n->n_exprs; i++)
        if (resolve_expr(n->exprs[i], tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    if (resolve_expr(n->agg_expr, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    if (n->kind == NODE_GROUPBY)
        if (resolve_one(&n->group_col, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    if (bind_rec(n->left, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    if (bind_rec(n->right, tables, ntables, cat, errbuf, errbuf_len) != 0) return -1;
    return 0;
}

int bind_columns(PlanNode *plan, Catalog *cat, char *errbuf, int errbuf_len) {
    char tables[MAX_TABLES][NAME_LEN];
    int ntables = plan_collect_tables(plan, tables, MAX_TABLES);
    return bind_rec(plan, tables, ntables, cat, errbuf, errbuf_len);
}
