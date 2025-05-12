#ifndef TINYOPT_PLAN_H
#define TINYOPT_PLAN_H

#include "value.h"

typedef enum {
    NODE_SCAN,
    NODE_FILTER,
    NODE_JOIN,
    NODE_CROSS,
    NODE_PROJECT,
    NODE_GROUPBY,
    NODE_LIMIT
} NodeKind;

typedef enum { OP_EQ, OP_LT, OP_LE, OP_GT, OP_GE, OP_NE } CmpOp;

typedef struct {
    char table[NAME_LEN];
    char col[NAME_LEN];
} ColRef;

typedef struct {
    int lhs_is_col;
    ColRef lhs_col;
    Value lhs_lit;
    CmpOp op;
    int rhs_is_col;
    ColRef rhs_col;
    Value rhs_lit;
} Pred;

typedef enum { EXPR_COL, EXPR_LIT, EXPR_ARITH, EXPR_AGG } ExprKind;
typedef enum { AGG_SUM, AGG_COUNT, AGG_AVG, AGG_MIN, AGG_MAX, AGG_NONE } AggKind;
typedef enum { ARITH_MUL, ARITH_ADD, ARITH_SUB, ARITH_DIV } ArithOp;

typedef struct Expr {
    ExprKind kind;
    ColRef col;
    Value lit;
    AggKind agg;
    ArithOp arith;
    struct Expr *left, *right;
    char alias[NAME_LEN];
} Expr;

#define MAX_PREDS 16
#define MAX_EXPRS 16

typedef struct PlanNode {
    NodeKind kind;

    char table[NAME_LEN];

    Pred preds[MAX_PREDS];
    int n_preds;

    Pred join_conds[MAX_PREDS];
    int n_join_conds;

    Expr *exprs[MAX_EXPRS];
    int n_exprs;
    int is_star;

    ColRef group_col;
    Expr *agg_expr;

    long limit_n;

    int always_false;

    struct PlanNode *left, *right;

    double est_cardinality;
    double est_cost;
    long actual_cardinality;
} PlanNode;

PlanNode *plan_new(NodeKind kind);
PlanNode *plan_scan(const char *table);
void plan_free(PlanNode *n);
PlanNode *plan_clone(const PlanNode *n);

int plan_references_table(const PlanNode *n, const char *t);
int plan_collect_tables(const PlanNode *n, char out[][NAME_LEN], int max);

void expr_free(Expr *e);
Expr *expr_clone(const Expr *e);
int expr_references_table(const Expr *e, const char *t);
void expr_collect_columns(const Expr *e, ColRef *out, int *n, int max);

const char *cmpop_str(CmpOp op);
const char *plan_kind_str(NodeKind k);

void plan_print(const PlanNode *n, int indent, int with_cost);

#endif
