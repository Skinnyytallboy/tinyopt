#include <string.h>
#include "test.h"
#include "parser.h"
#include "rewrite.h"
#include "plan.h"

static Pred lit_pred(long a, CmpOp op, long b) {
    Pred p; memset(&p, 0, sizeof(p));
    p.lhs_is_col = 0; p.lhs_lit = value_int(a);
    p.op = op;
    p.rhs_is_col = 0; p.rhs_lit = value_int(b);
    return p;
}

static void test_constant_fold_true(void) {
    PlanNode *f = plan_new(NODE_FILTER);
    f->left = plan_scan("t");
    f->preds[0] = lit_pred(2024, OP_EQ, 2024);
    f->n_preds = 1;
    PlanNode *r = rule_constant_fold(f);
    CHECK(r->n_preds == 0);
    CHECK(r->always_false == 0);
    plan_free(r);
}

static void test_constant_fold_false(void) {
    PlanNode *f = plan_new(NODE_FILTER);
    f->left = plan_scan("t");
    f->preds[0] = lit_pred(1, OP_EQ, 2);
    f->n_preds = 1;
    PlanNode *r = rule_constant_fold(f);
    CHECK(r->always_false == 1);
    plan_free(r);
}

static void test_predicate_pushdown_two_table(void) {
    char err[256];
    PlanNode *p = parse_query(
        "SELECT * FROM a, b WHERE a.id = b.a_id AND a.x = 5 AND b.y = 9", err, sizeof(err));
    CHECK(p != NULL);
    if (!p) return;

    p->left = rule_predicate_pushdown(p->left);

    /* top-level filter should be gone entirely -- everything pushed down */
    PlanNode *join = p->left;
    CHECK(join->kind == NODE_JOIN);
    CHECK(join->n_join_conds == 1);
    CHECK(strcmp(join->join_conds[0].lhs_col.col, "id") == 0 ||
          strcmp(join->join_conds[0].rhs_col.col, "id") == 0);

    PlanNode *left_side = join->left;
    CHECK(left_side->kind == NODE_FILTER);
    CHECK(left_side->n_preds == 1);
    CHECK(strcmp(left_side->preds[0].lhs_col.col, "x") == 0);

    PlanNode *right_side = join->right;
    CHECK(right_side->kind == NODE_FILTER);
    CHECK(right_side->n_preds == 1);
    CHECK(strcmp(right_side->preds[0].lhs_col.col, "y") == 0);

    plan_free(p);
}

void test_rewrite(void) {
    test_constant_fold_true();
    test_constant_fold_false();
    test_predicate_pushdown_two_table();
}
