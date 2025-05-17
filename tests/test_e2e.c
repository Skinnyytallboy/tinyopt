#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "test.h"
#include "catalog.h"
#include "parser.h"
#include "bind.h"
#include "rewrite.h"
#include "cost.h"
#include "joinorder.h"
#include "exec.h"

static const char *TESTDIR = "/tmp/tinyopt_e2e_test";

static void write_file(const char *name, const char *content) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", TESTDIR, name);
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

static void setup_data(void) {
    mkdir(TESTDIR, 0755);
    write_file("customers.csv",
        "id:INT,name:STRING,country:STRING\n"
        "1,Alice,US\n"
        "2,Bob,PK\n"
        "3,Cara,PK\n"
        "4,Deng,CN\n");
    write_file("orders.csv",
        "id:INT,customer_id:INT,total:DOUBLE\n"
        "1,1,50.00\n"
        "2,2,75.00\n"
        "3,2,20.00\n"
        "4,3,10.00\n"
        "5,4,90.00\n");
    /* force a fresh catalog scan */
    char cache[600]; snprintf(cache, sizeof(cache), "%s/catalog.json", TESTDIR);
    remove(cache);
}

static long run_and_count(const char *sql, Catalog *cat, int use_dp, int use_pushdown) {
    char err[256];
    PlanNode *p = parse_query(sql, err, sizeof(err));
    if (!p) { fprintf(stderr, "parse error: %s\n", err); return -1; }
    if (bind_columns(p, cat, err, sizeof(err)) != 0) { fprintf(stderr, "bind error: %s\n", err); plan_free(p); return -1; }

    if (use_pushdown) p = rewrite_to_fixpoint(p);
    else p = rule_attach_join_conditions(p);
    cost_estimate(p, cat);
    if (use_dp) {
        p = joinorder_optimize(p, cat, 0);
        cost_estimate(p, cat);
        rule_join_swap(p);
        cost_estimate(p, cat);
    }

    ExecConfig cfg = {0};
    Table *res = exec_plan(p, cat, TESTDIR, &cfg);
    long n = res->nrows;
    table_free(res);
    plan_free(p);
    return n;
}

static void test_naive_and_optimized_agree(void) {
    setup_data();
    Catalog cat; cat.use_histograms = 0;
    CHECK(catalog_load(&cat, TESTDIR) == 0);

    const char *sql = "SELECT * FROM customers, orders "
                       "WHERE customers.id = orders.customer_id AND customers.country = 'PK'";

    long naive_n = run_and_count(sql, &cat, 0, 0);
    long opt_n = run_and_count(sql, &cat, 1, 1);

    /* Bob has 2 orders, Cara has 1 -- 3 matching rows regardless of plan. */
    CHECK(naive_n == 3);
    CHECK(opt_n == 3);
    CHECK(naive_n == opt_n);
}

static void test_always_false_short_circuits(void) {
    setup_data();
    Catalog cat; cat.use_histograms = 0;
    CHECK(catalog_load(&cat, TESTDIR) == 0);

    char err[256];
    PlanNode *p = parse_query("SELECT * FROM customers WHERE 1 = 2", err, sizeof(err));
    CHECK(bind_columns(p, &cat, err, sizeof(err)) == 0);
    p = rewrite_to_fixpoint(p);
    cost_estimate(p, &cat);
    ExecConfig cfg = {0};
    Table *res = exec_plan(p, &cat, TESTDIR, &cfg);
    CHECK(res->nrows == 0);
    table_free(res);
    plan_free(p);
}

void test_e2e(void) {
    test_naive_and_optimized_agree();
    test_always_false_short_circuits();
}
