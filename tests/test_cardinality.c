#include <string.h>
#include <math.h>
#include "test.h"
#include "catalog.h"
#include "cost.h"

static void make_catalog(Catalog *cat) {
    memset(cat, 0, sizeof(*cat));
    cat->n_tables = 1;
    TableStats *t = &cat->tables[0];
    snprintf(t->name, NAME_LEN, "t");
    t->row_count = 1000;
    t->n_columns = 2;

    ColumnStats *c0 = &t->columns[0];
    snprintf(c0->name, NAME_LEN, "id");
    c0->type = VAL_INT;
    c0->distinct_count = 1000;
    c0->min_value = value_int(1);
    c0->max_value = value_int(1000);

    ColumnStats *c1 = &t->columns[1];
    snprintf(c1->name, NAME_LEN, "year");
    c1->type = VAL_INT;
    c1->distinct_count = 5;
    c1->min_value = value_int(2020);
    c1->max_value = value_int(2024);
}

static Pred eq_pred(const char *col, long lit) {
    Pred p; memset(&p, 0, sizeof(p));
    p.lhs_is_col = 1;
    snprintf(p.lhs_col.table, NAME_LEN, "t");
    snprintf(p.lhs_col.col, NAME_LEN, "%s", col);
    p.op = OP_EQ;
    p.rhs_is_col = 0;
    p.rhs_lit = value_int(lit);
    return p;
}

static void test_eq_selectivity(void) {
    Catalog cat; make_catalog(&cat);
    Pred p = eq_pred("year", 2024);
    double s = selectivity_of(&p, &cat);
    /* 1/distinct_count = 1/5 exactly */
    CHECK(fabs(s - 0.2) < 1e-9);
}

static void test_eq_out_of_range_selectivity(void) {
    Catalog cat; make_catalog(&cat);
    Pred p = eq_pred("year", 1999); /* below min */
    double s = selectivity_of(&p, &cat);
    CHECK(s == 0.0);
}

static void test_range_selectivity(void) {
    Catalog cat; make_catalog(&cat);
    Pred p; memset(&p, 0, sizeof(p));
    p.lhs_is_col = 1;
    snprintf(p.lhs_col.table, NAME_LEN, "t");
    snprintf(p.lhs_col.col, NAME_LEN, "id");
    p.op = OP_LT;
    p.rhs_is_col = 0;
    p.rhs_lit = value_int(500); /* midpoint of 1..1000 */
    double s = selectivity_of(&p, &cat);
    CHECK(s > 0.45 && s < 0.55);
}

void test_cardinality(void) {
    test_eq_selectivity();
    test_eq_out_of_range_selectivity();
    test_range_selectivity();
}
