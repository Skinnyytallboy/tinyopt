#include <string.h>
#include "test.h"
#include "parser.h"

static void test_simple_two_table(void) {
    char err[256];
    PlanNode *p = parse_query(
        "SELECT * FROM a, b WHERE a.id = b.a_id AND a.x = 5", err, sizeof(err));
    CHECK(p != NULL);
    if (!p) return;

    /* Project -> Filter -> Cross(Scan(a), Scan(b)) */
    CHECK(p->kind == NODE_PROJECT);
    CHECK(p->is_star == 1);
    PlanNode *filt = p->left;
    CHECK(filt && filt->kind == NODE_FILTER);
    CHECK(filt->n_preds == 2);
    PlanNode *join = filt->left;
    CHECK(join && (join->kind == NODE_CROSS || join->kind == NODE_JOIN));
    CHECK(join->left && join->left->kind == NODE_SCAN);
    CHECK(strcmp(join->left->table, "a") == 0);
    CHECK(join->right && join->right->kind == NODE_SCAN);
    CHECK(strcmp(join->right->table, "b") == 0);

    plan_free(p);
}

static void test_select_list_and_limit(void) {
    char err[256];
    PlanNode *p = parse_query("SELECT a.x, a.y FROM a LIMIT 10", err, sizeof(err));
    CHECK(p != NULL);
    if (!p) return;
    CHECK(p->kind == NODE_LIMIT);
    CHECK(p->limit_n == 10);
    PlanNode *proj = p->left;
    CHECK(proj && proj->kind == NODE_PROJECT);
    CHECK(proj->n_exprs == 2);
    CHECK(proj->exprs[0]->kind == EXPR_COL);
    CHECK(strcmp(proj->exprs[0]->col.col, "x") == 0);
    plan_free(p);
}

static void test_aggregate_and_group_by(void) {
    char err[256];
    PlanNode *p = parse_query(
        "SELECT a.g, SUM(a.v) FROM a GROUP BY a.g", err, sizeof(err));
    CHECK(p != NULL);
    if (!p) return;
    CHECK(p->kind == NODE_PROJECT);
    PlanNode *grp = p->left;
    CHECK(grp && grp->kind == NODE_GROUPBY);
    CHECK(strcmp(grp->group_col.col, "g") == 0);
    CHECK(grp->agg_expr != NULL);
    CHECK(grp->agg_expr->agg == AGG_SUM);
    plan_free(p);
}

static void test_parse_error(void) {
    char err[256];
    PlanNode *p = parse_query("SELEKT * FROM a", err, sizeof(err));
    CHECK(p == NULL);
}

static void test_range_predicate(void) {
    char err[256];
    PlanNode *p = parse_query("SELECT * FROM a WHERE a.x < 100", err, sizeof(err));
    CHECK(p != NULL);
    if (!p) return;
    PlanNode *filt = p->left;
    CHECK(filt->kind == NODE_FILTER);
    CHECK(filt->preds[0].op == OP_LT);
    plan_free(p);
}

void test_parser(void) {
    test_simple_two_table();
    test_select_list_and_limit();
    test_aggregate_and_group_by();
    test_parse_error();
    test_range_predicate();
}
