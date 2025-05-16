#include <string.h>
#include "test.h"
#include "catalog.h"
#include "parser.h"
#include "bind.h"
#include "rewrite.h"
#include "cost.h"
#include "joinorder.h"

/* small -(fk)- medium -(fk)- big, a classic chain. Joining small+medium
 * first keeps the intermediate tiny; joining big first (its natural
 * position if you just followed a bad FROM order) blows it up. The DP
 * should never do worse than the FROM order it started from. */
static void make_chain_catalog(Catalog *cat) {
    memset(cat, 0, sizeof(*cat));
    cat->n_tables = 3;

    TableStats *small = &cat->tables[0];
    snprintf(small->name, NAME_LEN, "small");
    small->row_count = 10;
    small->n_columns = 1;
    snprintf(small->columns[0].name, NAME_LEN, "id");
    small->columns[0].type = VAL_INT;
    small->columns[0].distinct_count = 10;
    small->columns[0].min_value = value_int(1);
    small->columns[0].max_value = value_int(10);

    TableStats *medium = &cat->tables[1];
    snprintf(medium->name, NAME_LEN, "medium");
    medium->row_count = 1000;
    medium->n_columns = 2;
    snprintf(medium->columns[0].name, NAME_LEN, "id");
    medium->columns[0].type = VAL_INT;
    medium->columns[0].distinct_count = 1000;
    medium->columns[0].min_value = value_int(1);
    medium->columns[0].max_value = value_int(1000);
    snprintf(medium->columns[1].name, NAME_LEN, "small_id");
    medium->columns[1].type = VAL_INT;
    medium->columns[1].distinct_count = 10;
    medium->columns[1].min_value = value_int(1);
    medium->columns[1].max_value = value_int(10);

    TableStats *big = &cat->tables[2];
    snprintf(big->name, NAME_LEN, "big");
    big->row_count = 100000;
    big->n_columns = 1;
    snprintf(big->columns[0].name, NAME_LEN, "medium_id");
    big->columns[0].type = VAL_INT;
    big->columns[0].distinct_count = 1000;
    big->columns[0].min_value = value_int(1);
    big->columns[0].max_value = value_int(1000);
}

void test_dp(void) {
    Catalog cat;
    make_chain_catalog(&cat);

    const char *sql = "SELECT * FROM big, medium, small "
                       "WHERE medium.id = big.medium_id AND small.id = medium.small_id";
    char err[256];

    PlanNode *naive = parse_query(sql, err, sizeof(err));
    CHECK(naive != NULL);
    if (!naive) return;
    CHECK(bind_columns(naive, &cat, err, sizeof(err)) == 0);
    naive = rule_attach_join_conditions(naive);
    cost_estimate(naive, &cat);
    double naive_cost = naive->est_cost;

    PlanNode *opt = parse_query(sql, err, sizeof(err));
    CHECK(bind_columns(opt, &cat, err, sizeof(err)) == 0);
    opt = rewrite_to_fixpoint(opt);
    cost_estimate(opt, &cat);
    opt = joinorder_optimize(opt, &cat, 0);
    cost_estimate(opt, &cat);

    /* the DP-chosen plan should be at least as good as the FROM-order
     * plan it started from -- that's the whole point of running it. */
    CHECK(opt->est_cost <= naive_cost);

    /* and it should actually have found the cheap chain order: joining
     * small+medium (tiny x tiny) before ever touching big, rather than
     * dragging big's 100,000 rows into an early join. */
    CHECK(opt->est_cost < naive_cost * 0.5);

    plan_free(naive);
    plan_free(opt);
}
