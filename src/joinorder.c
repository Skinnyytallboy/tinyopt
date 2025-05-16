#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "joinorder.h"
#include "cost.h"
#include "rewrite.h"

#define MAX_JOIN_TABLES 12
#define MAX_EDGES 64

typedef struct {
    char table[NAME_LEN];
    PlanNode *node;
} Leaf;

typedef struct {
    Pred cond;
    char ta[NAME_LEN], tb[NAME_LEN];
} Edge;

typedef struct {
    int valid;
    double cost;
    double card;
    PlanNode *plan;
} Cell;

static int contains_join_or_cross(PlanNode *n) {
    if (!n) return 0;
    if (n->kind == NODE_JOIN || n->kind == NODE_CROSS) return 1;
    return contains_join_or_cross(n->left) || contains_join_or_cross(n->right);
}

static int is_base_leaf(PlanNode *n) {
    return n->kind != NODE_JOIN && n->kind != NODE_CROSS && !contains_join_or_cross(n);
}

static void leaf_table_name(PlanNode *n, char *out) {
    char tables[MAX_TABLES][NAME_LEN];
    int nt = plan_collect_tables(n, tables, MAX_TABLES);
    snprintf(out, NAME_LEN, "%s", nt > 0 ? tables[0] : "");
}

static void extract_rec(PlanNode *n, Leaf *leaves, int *nleaves, Edge *edges, int *nedges) {
    if (is_base_leaf(n)) {
        leaf_table_name(n, leaves[*nleaves].table);
        leaves[*nleaves].node = n;
        (*nleaves)++;
        return;
    }
    extract_rec(n->left, leaves, nleaves, edges, nedges);
    extract_rec(n->right, leaves, nleaves, edges, nedges);
    for (int i = 0; i < n->n_join_conds && *nedges < MAX_EDGES; i++) {
        Edge *e = &edges[(*nedges)++];
        e->cond = n->join_conds[i];
        e->cond.lhs_lit = value_copy(&n->join_conds[i].lhs_lit);
        e->cond.rhs_lit = value_copy(&n->join_conds[i].rhs_lit);
        snprintf(e->ta, NAME_LEN, "%s", n->join_conds[i].lhs_is_col ? n->join_conds[i].lhs_col.table : "");
        snprintf(e->tb, NAME_LEN, "%s", n->join_conds[i].rhs_is_col ? n->join_conds[i].rhs_col.table : "");
    }
}

static void free_wrappers(PlanNode *n, Leaf *leaves, int nleaves) {
    if (!n) return;
    for (int i = 0; i < nleaves; i++) if (leaves[i].node == n) return;
    free_wrappers(n->left, leaves, nleaves);
    free_wrappers(n->right, leaves, nleaves);
    for (int i = 0; i < n->n_join_conds; i++) {
        value_free(&n->join_conds[i].lhs_lit);
        value_free(&n->join_conds[i].rhs_lit);
    }
    free(n);
}

static int mask_has_table(int mask, Leaf *leaves, int nl, const char *table) {
    for (int i = 0; i < nl; i++)
        if ((mask & (1 << i)) && strcmp(leaves[i].table, table) == 0) return 1;
    return 0;
}

static int find_connecting_edges(Edge *edges, int nedges, Leaf *leaves, int nl,
                                  int lmask, int rmask, int *out_idx, int max_out) {
    int n = 0;
    for (int i = 0; i < nedges && n < max_out; i++) {
        int a_left = mask_has_table(lmask, leaves, nl, edges[i].ta);
        int b_right = mask_has_table(rmask, leaves, nl, edges[i].tb);
        int a_right = mask_has_table(rmask, leaves, nl, edges[i].ta);
        int b_left = mask_has_table(lmask, leaves, nl, edges[i].tb);
        if ((a_left && b_right) || (a_right && b_left)) out_idx[n++] = i;
    }
    return n;
}

static PlanNode *build_join_node(Cell *L, Cell *R, Edge *edges, int *idx, int nidx) {
    PlanNode *j = plan_new(nidx > 0 ? NODE_JOIN : NODE_CROSS);
    j->left = L->plan;
    j->right = R->plan;
    j->n_join_conds = nidx;
    for (int i = 0; i < nidx; i++) {
        j->join_conds[i] = edges[idx[i]].cond;
        j->join_conds[i].lhs_lit = value_copy(&edges[idx[i]].cond.lhs_lit);
        j->join_conds[i].rhs_lit = value_copy(&edges[idx[i]].cond.rhs_lit);
    }
    return j;
}

static double join_card(Cell *L, Cell *R, Edge *edges, int *idx, int nidx, Catalog *cat) {
    if (nidx == 0) return L->card * R->card;
    double card = L->card * R->card;
    for (int i = 0; i < nidx; i++) card *= join_condition_selectivity(&edges[idx[i]].cond, cat);
    return card < 1.0 ? 1.0 : card;
}

static double join_cost(Cell *L, Cell *R, double card, int nidx) {
    if (nidx == 0) return L->cost + R->cost + L->card * R->card;
    return L->cost + R->cost + 2 * L->card + R->card + card;
}

PlanNode *joinorder_optimize(PlanNode *root, Catalog *cat, int allow_bushy) {
    PlanNode wrapper; memset(&wrapper, 0, sizeof(wrapper)); wrapper.left = root;
    PlanNode **slot = &wrapper.left;
    while (*slot && ((*slot)->kind == NODE_PROJECT || (*slot)->kind == NODE_GROUPBY ||
                     (*slot)->kind == NODE_LIMIT)) {
        slot = &(*slot)->left;
    }
    if (*slot && (*slot)->kind == NODE_FILTER && (*slot)->left &&
        ((*slot)->left->kind == NODE_JOIN || (*slot)->left->kind == NODE_CROSS)) {
        slot = &(*slot)->left;
    }

    if (!*slot || ((*slot)->kind != NODE_JOIN && (*slot)->kind != NODE_CROSS)) {
        return root;
    }

    Leaf leaves[MAX_JOIN_TABLES];
    Edge edges[MAX_EDGES];
    int nleaves = 0, nedges = 0;
    extract_rec(*slot, leaves, &nleaves, edges, &nedges);

    if (nleaves > MAX_JOIN_TABLES) {
        fprintf(stderr, "tinyopt: too many tables in one query for the DP (max %d)\n", MAX_JOIN_TABLES);
        return root;
    }

    for (int i = 0; i < nleaves; i++) cost_estimate(leaves[i].node, cat);

    int full = (1 << nleaves) - 1;
    Cell *dp = calloc((size_t)full + 1, sizeof(Cell));

    for (int i = 0; i < nleaves; i++) {
        int m = 1 << i;
        dp[m].valid = 1;
        dp[m].cost = leaves[i].node->est_cost;
        dp[m].card = leaves[i].node->est_cardinality;
        dp[m].plan = leaves[i].node;
    }

    for (int mask = 1; mask <= full; mask++) {
        if (dp[mask].valid) continue;
        if (__builtin_popcount(mask) < 2) continue;

        double best_cost = -1;
        int best_l = -1, best_r = -1;
        int best_idx[MAX_EDGES], best_nidx = 0;
        int found_connected = 0;

        for (int connected_only = 1; connected_only >= 0 && !found_connected; connected_only--) {
            if (!allow_bushy) {
                for (int i = 0; i < nleaves; i++) {
                    int rmask = 1 << i;
                    if (!(mask & rmask)) continue;
                    int lmask = mask & ~rmask;
                    if (!dp[lmask].valid) continue;
                    int idx[MAX_EDGES];
                    int nidx = find_connecting_edges(edges, nedges, leaves, nleaves, lmask, rmask, idx, MAX_EDGES);
                    if (connected_only && nidx == 0) continue;
                    double card = join_card(&dp[lmask], &dp[rmask], edges, idx, nidx, cat);
                    double cost = join_cost(&dp[lmask], &dp[rmask], card, nidx);
                    if (best_cost < 0 || cost < best_cost) {
                        best_cost = cost; best_l = lmask; best_r = rmask;
                        best_nidx = nidx;
                        memcpy(best_idx, idx, sizeof(int) * nidx);
                        if (nidx > 0) found_connected = 1;
                    }
                }
            } else {
                for (int rmask = mask; rmask > 0; rmask = (rmask - 1) & mask) {
                    if (rmask == mask) continue;
                    int lmask = mask & ~rmask;
                    if (lmask == 0) continue;
                    if (lmask < rmask) continue;
                    if (!dp[lmask].valid || !dp[rmask].valid) continue;
                    int idx[MAX_EDGES];
                    int nidx = find_connecting_edges(edges, nedges, leaves, nleaves, lmask, rmask, idx, MAX_EDGES);
                    if (connected_only && nidx == 0) continue;
                    double card = join_card(&dp[lmask], &dp[rmask], edges, idx, nidx, cat);
                    double cost = join_cost(&dp[lmask], &dp[rmask], card, nidx);
                    if (best_cost < 0 || cost < best_cost) {
                        best_cost = cost; best_l = lmask; best_r = rmask;
                        best_nidx = nidx;
                        memcpy(best_idx, idx, sizeof(int) * nidx);
                        if (nidx > 0) found_connected = 1;
                    }
                }
            }
        }

        if (best_l < 0) continue;

        double card = join_card(&dp[best_l], &dp[best_r], edges, best_idx, best_nidx, cat);
        PlanNode *node = build_join_node(&dp[best_l], &dp[best_r], edges, best_idx, best_nidx);
        node->est_cardinality = card;
        node->est_cost = best_cost;
        dp[mask].valid = 1;
        dp[mask].cost = best_cost;
        dp[mask].card = card;
        dp[mask].plan = node;
    }

    PlanNode *best_tree = dp[full].plan;

    free_wrappers(*slot, leaves, nleaves);
    *slot = best_tree;

    for (int i = 0; i < nedges; i++) {
        value_free(&edges[i].cond.lhs_lit);
        value_free(&edges[i].cond.rhs_lit);
    }
    free(dp);

    return wrapper.left;
}
