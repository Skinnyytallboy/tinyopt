#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exec.h"

#define AGG_RESULT_COL "agg_result"

Table *table_new(void) {
    Table *t = calloc(1, sizeof(Table));
    return t;
}

static void table_grow(Table *t) {
    if (t->nrows == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->rows = realloc(t->rows, t->cap * sizeof(Value *));
    }
}

static void table_append_row(Table *t, Value *row) {
    table_grow(t);
    t->rows[t->nrows++] = row;
}

void table_free(Table *t) {
    if (!t) return;
    for (long i = 0; i < t->nrows; i++) {
        for (int j = 0; j < t->schema.ncols; j++) value_free(&t->rows[i][j]);
        free(t->rows[i]);
    }
    free(t->rows);
    free(t);
}

int schema_find(const Schema *s, const char *table, const char *col) {
    for (int i = 0; i < s->ncols; i++) {
        if (strcmp(s->cols[i].col, col) != 0) continue;
        if (table[0] == 0 || strcmp(s->cols[i].table, table) == 0) return i;
    }
    return -1;
}

static const char *agg_name(AggKind k) {
    switch (k) {
        case AGG_SUM: return "SUM"; case AGG_COUNT: return "COUNT"; case AGG_AVG: return "AVG";
        case AGG_MIN: return "MIN"; case AGG_MAX: return "MAX"; default: return "AGG";
    }
}

static Schema build_output_schema(PlanNode *n, Catalog *cat) {
    Schema s; memset(&s, 0, sizeof(s));
    switch (n->kind) {
    case NODE_SCAN: {
        TableStats *ts = catalog_find_table(cat, n->table);
        if (ts) for (int i = 0; i < ts->n_columns && s.ncols < MAX_SCHEMA_COLS; i++) {
            snprintf(s.cols[s.ncols].table, NAME_LEN, "%s", n->table);
            snprintf(s.cols[s.ncols].col, NAME_LEN, "%s", ts->columns[i].name);
            s.ncols++;
        }
        break;
    }
    case NODE_FILTER:
    case NODE_LIMIT:
        s = build_output_schema(n->left, cat);
        break;
    case NODE_JOIN:
    case NODE_CROSS: {
        Schema l = build_output_schema(n->left, cat), r = build_output_schema(n->right, cat);
        s = l;
        for (int i = 0; i < r.ncols && s.ncols < MAX_SCHEMA_COLS; i++) s.cols[s.ncols++] = r.cols[i];
        break;
    }
    case NODE_PROJECT: {
        if (n->is_star) { s = build_output_schema(n->left, cat); break; }
        for (int i = 0; i < n->n_exprs && s.ncols < MAX_SCHEMA_COLS; i++) {
            Expr *e = n->exprs[i];
            if (e->kind == EXPR_COL) {
                s.cols[s.ncols++] = e->col;
            } else if (e->kind == EXPR_AGG) {
                snprintf(s.cols[s.ncols].table, NAME_LEN, "%s", "");
                snprintf(s.cols[s.ncols].col, NAME_LEN, "%s_%s", agg_name(e->agg), e->col.col);
                s.ncols++;
            } else {
                snprintf(s.cols[s.ncols].table, NAME_LEN, "%s", "");
                snprintf(s.cols[s.ncols].col, NAME_LEN, "expr_result");
                s.ncols++;
            }
        }
        break;
    }
    case NODE_GROUPBY: {
        s.cols[s.ncols++] = n->group_col;
        snprintf(s.cols[s.ncols].table, NAME_LEN, "%s", "");
        snprintf(s.cols[s.ncols].col, NAME_LEN, "%s", AGG_RESULT_COL);
        s.ncols++;
        break;
    }
    }
    return s;
}

static Value eval_expr(Expr *e, Value *row, Schema *sch) {
    if (!e) return value_int(0);
    switch (e->kind) {
    case EXPR_LIT:
        return value_copy(&e->lit);
    case EXPR_COL: {
        int idx = schema_find(sch, e->col.table, e->col.col);
        if (idx < 0) return value_int(0);
        return value_copy(&row[idx]);
    }
    case EXPR_ARITH: {
        Value l = eval_expr(e->left, row, sch);
        Value r = eval_expr(e->right, row, sch);
        double dl = value_to_double(&l), dr = value_to_double(&r);
        value_free(&l); value_free(&r);
        switch (e->arith) {
            case ARITH_MUL: return value_double(dl * dr);
            case ARITH_ADD: return value_double(dl + dr);
            case ARITH_SUB: return value_double(dl - dr);
            case ARITH_DIV: return value_double(dr != 0 ? dl / dr : 0.0);
        }
        return value_double(0);
    }
    case EXPR_AGG: {
        int idx = schema_find(sch, "", AGG_RESULT_COL);
        if (idx >= 0) return value_copy(&row[idx]);
        if (e->left) return eval_expr(e->left, row, sch);
        int cidx = schema_find(sch, e->col.table, e->col.col);
        if (cidx < 0) return value_int(0);
        return value_copy(&row[cidx]);
    }
    }
    return value_int(0);
}

static Value fetch_pred_side(int is_col, const ColRef *col, const Value *lit, Value *row, Schema *sch) {
    if (is_col) {
        int idx = schema_find(sch, col->table, col->col);
        if (idx < 0) return value_int(0);
        return value_copy(&row[idx]);
    }
    return value_copy(lit);
}

static int eval_pred(const Pred *p, Value *row, Schema *sch) {
    Value l = fetch_pred_side(p->lhs_is_col, &p->lhs_col, &p->lhs_lit, row, sch);
    Value r = fetch_pred_side(p->rhs_is_col, &p->rhs_col, &p->rhs_lit, row, sch);
    int c = value_compare(&l, &r);
    value_free(&l); value_free(&r);
    switch (p->op) {
        case OP_EQ: return c == 0;
        case OP_NE: return c != 0;
        case OP_LT: return c < 0;
        case OP_LE: return c <= 0;
        case OP_GT: return c > 0;
        case OP_GE: return c >= 0;
    }
    return 0;
}

static int row_matches_all(PlanNode *n, Value *row, Schema *sch) {
    for (int i = 0; i < n->n_preds; i++)
        if (!eval_pred(&n->preds[i], row, sch)) return 0;
    return 1;
}

static Value *row_copy(Value *row, int ncols) {
    Value *r = malloc(ncols * sizeof(Value));
    for (int i = 0; i < ncols; i++) r[i] = value_copy(&row[i]);
    return r;
}

static void trim_nl(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = 0;
}

static Table *exec_scan(PlanNode *n, Catalog *cat) {
    Table *t = table_new();
    TableStats *ts = catalog_find_table(cat, n->table);
    if (!ts) return t;
    t->schema = build_output_schema(n, cat);

    FILE *f = fopen(ts->csv_path, "r");
    if (!f) return t;
    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return t; }
    while (fgets(line, sizeof(line), f)) {
        trim_nl(line);
        if (line[0] == 0) continue;
        Value *row = malloc(ts->n_columns * sizeof(Value));
        char *saveptr;
        char *field = strtok_r(line, ",", &saveptr);
        for (int c = 0; c < ts->n_columns; c++) {
            if (!field) { row[c] = value_int(0); continue; }
            switch (ts->columns[c].type) {
                case VAL_INT: row[c] = value_int(atoll(field)); break;
                case VAL_DOUBLE: row[c] = value_double(atof(field)); break;
                case VAL_STRING: row[c] = value_string(field); break;
            }
            field = strtok_r(NULL, ",", &saveptr);
        }
        table_append_row(t, row);
    }
    fclose(f);
    return t;
}

static Table *exec_filter(PlanNode *n, Table *child) {
    Table *t = table_new();
    t->schema = child->schema;
    for (long i = 0; i < child->nrows; i++) {
        if (row_matches_all(n, child->rows[i], &child->schema))
            table_append_row(t, row_copy(child->rows[i], child->schema.ncols));
    }
    return t;
}

static Table *exec_cross(Table *left, Table *right) {
    Table *t = table_new();
    t->schema = left->schema;
    for (int i = 0; i < right->schema.ncols && t->schema.ncols < MAX_SCHEMA_COLS; i++)
        t->schema.cols[t->schema.ncols++] = right->schema.cols[i];

    int lc = left->schema.ncols, rc = right->schema.ncols;
    for (long i = 0; i < left->nrows; i++) {
        for (long j = 0; j < right->nrows; j++) {
            Value *row = malloc((lc + rc) * sizeof(Value));
            for (int k = 0; k < lc; k++) row[k] = value_copy(&left->rows[i][k]);
            for (int k = 0; k < rc; k++) row[lc + k] = value_copy(&right->rows[j][k]);
            table_append_row(t, row);
        }
    }
    return t;
}

/* ---------------- hash join ---------------- */

#define HASH_BUCKETS 4093

typedef struct HNode { long row_idx; struct HNode *next; } HNode;

static unsigned long hash_value(const Value *v) {
    char buf[128];
    value_print(v, buf, sizeof(buf));
    unsigned long h = 1469598103934665603UL;
    for (char *p = buf; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211UL; }
    return h;
}

static void cond_sides(const Pred *p, Schema *lsch, Schema *rsch,
                        int *left_idx, int *right_idx) {
    int a_in_l = schema_find(lsch, p->lhs_col.table, p->lhs_col.col);
    int a_in_r = schema_find(rsch, p->lhs_col.table, p->lhs_col.col);
    if (a_in_l >= 0) {
        *left_idx = a_in_l;
        *right_idx = schema_find(rsch, p->rhs_col.table, p->rhs_col.col);
    } else {
        *left_idx = schema_find(lsch, p->rhs_col.table, p->rhs_col.col);
        *right_idx = a_in_r;
    }
}

static Table *exec_hash_join(PlanNode *n, Table *left, Table *right) {
    Table *t = table_new();
    t->schema = left->schema;
    for (int i = 0; i < right->schema.ncols && t->schema.ncols < MAX_SCHEMA_COLS; i++)
        t->schema.cols[t->schema.ncols++] = right->schema.cols[i];

    int lidx[MAX_PREDS], ridx[MAX_PREDS];
    for (int i = 0; i < n->n_join_conds; i++)
        cond_sides(&n->join_conds[i], &left->schema, &right->schema, &lidx[i], &ridx[i]);

    HNode **buckets = calloc(HASH_BUCKETS, sizeof(HNode *));
    for (long i = 0; i < left->nrows; i++) {
        unsigned long h = hash_value(&left->rows[i][lidx[0]]) % HASH_BUCKETS;
        HNode *hn = malloc(sizeof(HNode));
        hn->row_idx = i;
        hn->next = buckets[h];
        buckets[h] = hn;
    }

    int lc = left->schema.ncols, rc = right->schema.ncols;
    for (long j = 0; j < right->nrows; j++) {
        unsigned long h = hash_value(&right->rows[j][ridx[0]]) % HASH_BUCKETS;
        for (HNode *hn = buckets[h]; hn; hn = hn->next) {
            long i = hn->row_idx;
            int ok = 1;
            for (int c = 0; c < n->n_join_conds; c++) {
                if (value_compare(&left->rows[i][lidx[c]], &right->rows[j][ridx[c]]) != 0) { ok = 0; break; }
            }
            if (!ok) continue;
            Value *row = malloc((lc + rc) * sizeof(Value));
            for (int k = 0; k < lc; k++) row[k] = value_copy(&left->rows[i][k]);
            for (int k = 0; k < rc; k++) row[lc + k] = value_copy(&right->rows[j][k]);
            table_append_row(t, row);
        }
    }

    for (int b = 0; b < HASH_BUCKETS; b++) {
        HNode *hn = buckets[b];
        while (hn) { HNode *nx = hn->next; free(hn); hn = nx; }
    }
    free(buckets);
    return t;
}

typedef struct { Table *tab; int key_idx; } SortCtx;
static SortCtx g_sort_ctx;

static int cmp_by_key(const void *a, const void *b) {
    long ia = *(const long *)a, ib = *(const long *)b;
    return value_compare(&g_sort_ctx.tab->rows[ia][g_sort_ctx.key_idx],
                          &g_sort_ctx.tab->rows[ib][g_sort_ctx.key_idx]);
}

static long *sorted_row_order(Table *tab, int key_idx) {
    long *order = malloc(tab->nrows * sizeof(long));
    for (long i = 0; i < tab->nrows; i++) order[i] = i;
    g_sort_ctx.tab = tab; g_sort_ctx.key_idx = key_idx;
    qsort(order, tab->nrows, sizeof(long), cmp_by_key);
    return order;
}

static Table *exec_sortmerge_join(PlanNode *n, Table *left, Table *right) {
    if (n->n_join_conds != 1 || n->join_conds[0].op != OP_EQ)
        return exec_hash_join(n, left, right);

    Table *t = table_new();
    t->schema = left->schema;
    for (int i = 0; i < right->schema.ncols && t->schema.ncols < MAX_SCHEMA_COLS; i++)
        t->schema.cols[t->schema.ncols++] = right->schema.cols[i];

    int lidx, ridx;
    cond_sides(&n->join_conds[0], &left->schema, &right->schema, &lidx, &ridx);

    long *lorder = sorted_row_order(left, lidx);
    long *rorder = sorted_row_order(right, ridx);

    int lc = left->schema.ncols, rc = right->schema.ncols;
    long i = 0, j = 0;
    while (i < left->nrows && j < right->nrows) {
        long li = lorder[i], rj = rorder[j];
        int c = value_compare(&left->rows[li][lidx], &right->rows[rj][ridx]);
        if (c < 0) { i++; continue; }
        if (c > 0) { j++; continue; }

        long i_end = i;
        while (i_end < left->nrows &&
               value_compare(&left->rows[lorder[i_end]][lidx], &left->rows[li][lidx]) == 0) i_end++;
        long j_end = j;
        while (j_end < right->nrows &&
               value_compare(&right->rows[rorder[j_end]][ridx], &right->rows[rj][ridx]) == 0) j_end++;

        for (long a = i; a < i_end; a++) {
            for (long b = j; b < j_end; b++) {
                long la = lorder[a], rb = rorder[b];
                Value *row = malloc((lc + rc) * sizeof(Value));
                for (int k = 0; k < lc; k++) row[k] = value_copy(&left->rows[la][k]);
                for (int k = 0; k < rc; k++) row[lc + k] = value_copy(&right->rows[rb][k]);
                table_append_row(t, row);
            }
        }
        i = i_end; j = j_end;
    }

    free(lorder); free(rorder);
    return t;
}

static int is_pure_cross_chain(PlanNode *n) {
    if (!n) return 0;
    if (n->kind == NODE_SCAN) return 1;
    if (n->kind == NODE_FILTER) return n->left && n->left->kind == NODE_SCAN && is_pure_cross_chain(n->left);
    if (n->kind == NODE_CROSS) return is_pure_cross_chain(n->left) && is_pure_cross_chain(n->right);
    return 0;
}

static void collect_leaves(PlanNode *n, PlanNode **leaves, int *nleaves) {
    if (n->kind == NODE_SCAN || n->kind == NODE_FILTER) { leaves[(*nleaves)++] = n; return; }
    collect_leaves(n->left, leaves, nleaves);
    collect_leaves(n->right, leaves, nleaves);
}

typedef struct {
    PlanNode *filter_node;
    Table **tabs;
    int ntabs;
    Schema combined;
    long *idx;
    Value *temp_row;
    Table *result;
} LazyCrossCtx;

static void lazy_loop(LazyCrossCtx *ctx, int depth) {
    if (depth == ctx->ntabs) {
        int pos = 0;
        for (int t = 0; t < ctx->ntabs; t++) {
            Table *tb = ctx->tabs[t];
            Value *r = tb->rows[ctx->idx[t]];
            for (int c = 0; c < tb->schema.ncols; c++) ctx->temp_row[pos++] = r[c];
        }
        if (!ctx->filter_node || row_matches_all(ctx->filter_node, ctx->temp_row, &ctx->combined)) {
            Value *row = malloc(ctx->combined.ncols * sizeof(Value));
            for (int c = 0; c < ctx->combined.ncols; c++) row[c] = value_copy(&ctx->temp_row[c]);
            table_append_row(ctx->result, row);
        }
        return;
    }
    for (long i = 0; i < ctx->tabs[depth]->nrows; i++) {
        ctx->idx[depth] = i;
        lazy_loop(ctx, depth + 1);
    }
}

static Table *exec_lazy_cross_filter(PlanNode *filter_node, PlanNode *cross_root, Catalog *cat,
                                     const char *data_dir, ExecConfig *cfg) {
    PlanNode *leaves[MAX_TABLES];
    int nleaves = 0;
    collect_leaves(cross_root, leaves, &nleaves);

    LazyCrossCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.ntabs = nleaves;
    ctx.tabs = malloc(nleaves * sizeof(Table *));
    for (int i = 0; i < nleaves; i++) ctx.tabs[i] = exec_plan(leaves[i], cat, data_dir, cfg);

    ctx.combined.ncols = 0;
    for (int i = 0; i < nleaves; i++)
        for (int c = 0; c < ctx.tabs[i]->schema.ncols && ctx.combined.ncols < MAX_SCHEMA_COLS; c++)
            ctx.combined.cols[ctx.combined.ncols++] = ctx.tabs[i]->schema.cols[c];

    ctx.filter_node = filter_node;
    ctx.idx = calloc((size_t)nleaves, sizeof(long));
    ctx.temp_row = malloc(ctx.combined.ncols * sizeof(Value));
    ctx.result = table_new();
    ctx.result->schema = ctx.combined;

    if (nleaves > 0 && ctx.tabs[0]) {
        int any_empty = 0;
        for (int i = 0; i < nleaves; i++) if (ctx.tabs[i]->nrows == 0) any_empty = 1;
        if (!any_empty) lazy_loop(&ctx, 0);
    }

    for (int i = 0; i < nleaves; i++) table_free(ctx.tabs[i]);
    free(ctx.tabs);
    free(ctx.idx);
    free(ctx.temp_row);
    return ctx.result;
}

static Table *exec_project(PlanNode *n, Table *child) {
    if (n->is_star) return child;

    Table *t = table_new();
    t->schema.ncols = 0;
    for (int i = 0; i < n->n_exprs; i++) {
        Expr *e = n->exprs[i];
        if (e->kind == EXPR_COL) {
            t->schema.cols[t->schema.ncols++] = e->col;
        } else if (e->kind == EXPR_AGG) {
            snprintf(t->schema.cols[t->schema.ncols].table, NAME_LEN, "%s", "");
            snprintf(t->schema.cols[t->schema.ncols].col, NAME_LEN, "%s_%s", agg_name(e->agg), e->col.col);
            t->schema.ncols++;
        } else {
            snprintf(t->schema.cols[t->schema.ncols].table, NAME_LEN, "%s", "");
            snprintf(t->schema.cols[t->schema.ncols].col, NAME_LEN, "expr_result");
            t->schema.ncols++;
        }
    }

    for (long i = 0; i < child->nrows; i++) {
        Value *row = malloc(n->n_exprs * sizeof(Value));
        for (int e = 0; e < n->n_exprs; e++) row[e] = eval_expr(n->exprs[e], child->rows[i], &child->schema);
        table_append_row(t, row);
    }
    table_free(child);
    return t;
}

typedef struct {
    Value key;
    double sum;
    long count;
    double minv, maxv;
    int has_val;
} GroupState;

static Table *exec_groupby(PlanNode *n, Table *child) {
    Table *t = table_new();
    t->schema.ncols = 0;
    t->schema.cols[t->schema.ncols++] = n->group_col;
    snprintf(t->schema.cols[t->schema.ncols].table, NAME_LEN, "%s", "");
    snprintf(t->schema.cols[t->schema.ncols].col, NAME_LEN, "%s", AGG_RESULT_COL);
    t->schema.ncols++;

    int gidx = schema_find(&child->schema, n->group_col.table, n->group_col.col);

    GroupState *groups = NULL;
    long ngroups = 0, cap = 0;

    Expr *arg_expr = n->agg_expr && n->agg_expr->left ? n->agg_expr->left : NULL;

    for (long i = 0; i < child->nrows; i++) {
        Value key = gidx >= 0 ? child->rows[i][gidx] : value_int(0);
        long g = -1;
        for (long k = 0; k < ngroups; k++) {
            if (value_compare(&groups[k].key, &key) == 0) { g = k; break; }
        }
        if (g < 0) {
            if (ngroups == cap) { cap = cap ? cap * 2 : 32; groups = realloc(groups, cap * sizeof(GroupState)); }
            g = ngroups++;
            groups[g].key = value_copy(&key);
            groups[g].sum = 0; groups[g].count = 0; groups[g].has_val = 0;
            groups[g].minv = 0; groups[g].maxv = 0;
        }
        double v = 0;
        if (n->agg_expr) {
            if (arg_expr) {
                Value vv = eval_expr(arg_expr, child->rows[i], &child->schema);
                v = value_to_double(&vv);
                value_free(&vv);
            } else {
                int aidx = schema_find(&child->schema, n->agg_expr->col.table, n->agg_expr->col.col);
                if (aidx >= 0) v = value_to_double(&child->rows[i][aidx]);
            }
        }
        groups[g].sum += v;
        groups[g].count += 1;
        if (!groups[g].has_val) { groups[g].minv = groups[g].maxv = v; groups[g].has_val = 1; }
        else { if (v < groups[g].minv) groups[g].minv = v; if (v > groups[g].maxv) groups[g].maxv = v; }
    }

    AggKind ak = n->agg_expr ? n->agg_expr->agg : AGG_COUNT;
    for (long g = 0; g < ngroups; g++) {
        Value *row = malloc(2 * sizeof(Value));
        row[0] = groups[g].key;
        double result;
        switch (ak) {
            case AGG_SUM: result = groups[g].sum; break;
            case AGG_COUNT: result = (double)groups[g].count; break;
            case AGG_AVG: result = groups[g].count ? groups[g].sum / groups[g].count : 0; break;
            case AGG_MIN: result = groups[g].minv; break;
            case AGG_MAX: result = groups[g].maxv; break;
            default: result = 0;
        }
        row[1] = value_double(result);
        table_append_row(t, row);
    }
    free(groups);
    table_free(child);
    return t;
}

static Table *exec_limit(PlanNode *n, Table *child) {
    Table *t = table_new();
    t->schema = child->schema;
    long lim = n->limit_n < 0 ? child->nrows : n->limit_n;
    for (long i = 0; i < child->nrows && i < lim; i++)
        table_append_row(t, row_copy(child->rows[i], child->schema.ncols));
    table_free(child);
    return t;
}

Table *exec_plan(PlanNode *n, Catalog *cat, const char *data_dir, ExecConfig *cfg) {
    (void)data_dir;
    Table *result = NULL;
    switch (n->kind) {
    case NODE_SCAN:
        result = exec_scan(n, cat);
        break;
    case NODE_FILTER:
        if (n->always_false) {
            result = table_new();
            result->schema = build_output_schema(n->left, cat);
        } else if (n->left && n->left->kind == NODE_CROSS && is_pure_cross_chain(n->left)) {
            result = exec_lazy_cross_filter(n, n->left, cat, data_dir, cfg);
        } else {
            Table *child = exec_plan(n->left, cat, data_dir, cfg);
            result = exec_filter(n, child);
            table_free(child);
        }
        break;
    case NODE_CROSS: {
        if (is_pure_cross_chain(n)) {
            result = exec_lazy_cross_filter(NULL, n, cat, data_dir, cfg);
            break;
        }
        Table *l = exec_plan(n->left, cat, data_dir, cfg);
        Table *r = exec_plan(n->right, cat, data_dir, cfg);
        result = exec_cross(l, r);
        table_free(l); table_free(r);
        break;
    }
    case NODE_JOIN: {
        Table *l = exec_plan(n->left, cat, data_dir, cfg);
        Table *r = exec_plan(n->right, cat, data_dir, cfg);
        JoinAlgo algo = cfg ? cfg->join_algo : JOIN_HASH;
        result = algo == JOIN_SORTMERGE ? exec_sortmerge_join(n, l, r) : exec_hash_join(n, l, r);
        table_free(l); table_free(r);
        break;
    }
    case NODE_PROJECT: {
        Table *child = exec_plan(n->left, cat, data_dir, cfg);
        result = exec_project(n, child);
        break;
    }
    case NODE_GROUPBY: {
        Table *child = exec_plan(n->left, cat, data_dir, cfg);
        result = exec_groupby(n, child);
        break;
    }
    case NODE_LIMIT: {
        Table *child = exec_plan(n->left, cat, data_dir, cfg);
        result = exec_limit(n, child);
        break;
    }
    }
    n->actual_cardinality = result->nrows;
    if (cfg && cfg->trace)
        fprintf(stderr, "  %-14s -> %ld rows\n", plan_kind_str(n->kind), result->nrows);
    return result;
}
