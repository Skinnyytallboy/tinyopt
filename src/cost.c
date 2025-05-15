#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cost.h"

static double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static double hist_range_selectivity(ColumnStats *cs, double lit, CmpOp op) {
    int nb = cs->n_buckets;
    double per_bucket = 1.0 / nb;
    double below = 0.0;
    for (int i = 0; i < nb; i++) {
        double lo = cs->bucket_bounds[i], hi = cs->bucket_bounds[i + 1];
        double frac;
        if (hi <= lo) frac = (lo < lit) ? 1.0 : 0.0;
        else frac = clamp01((lit - lo) / (hi - lo));
        below += per_bucket * frac;
    }
    below = clamp01(below);
    switch (op) {
        case OP_LT: return below;
        case OP_LE: return below;
        case OP_GT: return clamp01(1.0 - below);
        case OP_GE: return clamp01(1.0 - below);
        default: return 0.5;
    }
}

static double minmax_range_selectivity(ColumnStats *cs, double lit, CmpOp op) {
    double mn = value_to_double(&cs->min_value);
    double mx = value_to_double(&cs->max_value);
    if (mx <= mn) return 0.5;
    double frac_below = clamp01((lit - mn) / (mx - mn));
    switch (op) {
        case OP_LT: case OP_LE: return frac_below;
        case OP_GT: case OP_GE: return clamp01(1.0 - frac_below);
        default: return 0.5;
    }
}

double selectivity_of(const Pred *p, Catalog *cat) {
    if (p->lhs_is_col && !p->rhs_is_col) {
        TableStats *ts = catalog_find_table(cat, p->lhs_col.table);
        ColumnStats *cs = ts ? catalog_find_column(ts, p->lhs_col.col) : NULL;
        if (!cs) return 0.1;

        if (p->op == OP_EQ) {
            if (cs->type != VAL_STRING) {
                double lit = value_to_double(&p->rhs_lit);
                double mn = value_to_double(&cs->min_value), mx = value_to_double(&cs->max_value);
                if (lit < mn || lit > mx) return 0.0;
            }
            return 1.0 / (double)cs->distinct_count;
        }
        if (p->op == OP_NE) return 1.0 - 1.0 / (double)cs->distinct_count;

        if (cs->type == VAL_STRING) return 0.3;
        double lit = value_to_double(&p->rhs_lit);
        if (cs->has_histogram) return hist_range_selectivity(cs, lit, p->op);
        return minmax_range_selectivity(cs, lit, p->op);
    }

    if (p->rhs_is_col && !p->lhs_is_col) {
        Pred flipped = *p;
        flipped.lhs_is_col = 1; flipped.lhs_col = p->rhs_col;
        flipped.rhs_is_col = 0; flipped.rhs_lit = p->lhs_lit;
        switch (p->op) {
            case OP_LT: flipped.op = OP_GT; break;
            case OP_LE: flipped.op = OP_GE; break;
            case OP_GT: flipped.op = OP_LT; break;
            case OP_GE: flipped.op = OP_LE; break;
            default: flipped.op = p->op; break;
        }
        return selectivity_of(&flipped, cat);
    }

    if (p->lhs_is_col && p->rhs_is_col) {
        if (p->op == OP_EQ) {
            TableStats *ta = catalog_find_table(cat, p->lhs_col.table);
            TableStats *tb = catalog_find_table(cat, p->rhs_col.table);
            ColumnStats *ca = ta ? catalog_find_column(ta, p->lhs_col.col) : NULL;
            ColumnStats *cb = tb ? catalog_find_column(tb, p->rhs_col.col) : NULL;
            long da = ca ? ca->distinct_count : 100, db = cb ? cb->distinct_count : 100;
            long mx = da > db ? da : db;
            return 1.0 / (double)mx;
        }
        return 0.3;
    }

    return 0.3;
}

static double filter_selectivity(const PlanNode *n, Catalog *cat) {
    double sel = 1.0;
    for (int i = 0; i < n->n_preds; i++) sel *= selectivity_of(&n->preds[i], cat);
    return sel;
}

double join_condition_selectivity(const Pred *p, Catalog *cat) {
    TableStats *ta = catalog_find_table(cat, p->lhs_col.table);
    TableStats *tb = catalog_find_table(cat, p->rhs_col.table);
    ColumnStats *ca = ta ? catalog_find_column(ta, p->lhs_col.col) : NULL;
    ColumnStats *cb = tb ? catalog_find_column(tb, p->rhs_col.col) : NULL;
    long da = ca ? ca->distinct_count : 100;
    long db = cb ? cb->distinct_count : 100;
    long denom = da > db ? da : db;
    if (denom < 1) denom = 1;
    return 1.0 / (double)denom;
}

void cost_estimate(PlanNode *n, Catalog *cat) {
    if (!n) return;
    cost_estimate(n->left, cat);
    cost_estimate(n->right, cat);

    switch (n->kind) {
    case NODE_SCAN: {
        TableStats *ts = catalog_find_table(cat, n->table);
        n->est_cardinality = ts ? (double)ts->row_count : 1000.0;
        n->est_cost = n->est_cardinality;
        break;
    }
    case NODE_FILTER: {
        if (n->always_false) {
            n->est_cardinality = 0.0;
            n->est_cost = n->left->est_cost + n->left->est_cardinality;
            break;
        }
        double sel = filter_selectivity(n, cat);
        n->est_cardinality = n->left->est_cardinality * sel;
        if (n->est_cardinality < 0.01 && n->n_preds > 0) n->est_cardinality = 0.01;
        n->est_cost = n->left->est_cost + n->left->est_cardinality;
        break;
    }
    case NODE_CROSS: {
        n->est_cardinality = n->left->est_cardinality * n->right->est_cardinality;
        n->est_cost = n->left->est_cost + n->right->est_cost +
                      n->left->est_cardinality * n->right->est_cardinality;
        break;
    }
    case NODE_JOIN: {
        double card;
        if (n->n_join_conds == 0) {
            card = n->left->est_cardinality * n->right->est_cardinality;
        } else {
            card = n->left->est_cardinality * n->right->est_cardinality *
                   join_condition_selectivity(&n->join_conds[0], cat);
            for (int i = 1; i < n->n_join_conds; i++)
                card *= join_condition_selectivity(&n->join_conds[i], cat);
        }
        if (card < 1.0) card = 1.0;
        n->est_cardinality = card;
        n->est_cost = n->left->est_cost + n->right->est_cost +
                      2 * n->left->est_cardinality + n->right->est_cardinality +
                      n->est_cardinality;
        break;
    }
    case NODE_PROJECT: {
        n->est_cardinality = n->left ? n->left->est_cardinality : 0;
        n->est_cost = n->left ? n->left->est_cost + n->left->est_cardinality : 0;
        break;
    }
    case NODE_GROUPBY: {
        TableStats *ts = catalog_find_table(cat, n->group_col.table);
        ColumnStats *cs = ts ? catalog_find_column(ts, n->group_col.col) : NULL;
        double dc = cs ? (double)cs->distinct_count : 10.0;
        n->est_cardinality = dc < n->left->est_cardinality ? dc : n->left->est_cardinality;
        n->est_cost = n->left->est_cost + n->left->est_cardinality;
        break;
    }
    case NODE_LIMIT: {
        n->est_cardinality = n->left->est_cardinality;
        if (n->limit_n >= 0 && n->limit_n < n->est_cardinality) n->est_cardinality = n->limit_n;
        n->est_cost = n->left->est_cost + n->left->est_cardinality;
        break;
    }
    }
}
