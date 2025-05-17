#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "catalog.h"
#include "parser.h"
#include "bind.h"
#include "rewrite.h"
#include "cost.h"
#include "joinorder.h"
#include "exec.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct {
    const char *label;
    const char *sql;
} BenchQuery;

static BenchQuery QUERIES[] = {
    { "Q1 (two-table, selective)",
      "SELECT * FROM customers, orders WHERE customers.id = orders.customer_id AND customers.country = 'PK'" },
    { "Q2 (three-table, selective)",
      "SELECT customers.name, SUM(line_items.qty * line_items.price) FROM customers, orders, line_items "
      "WHERE customers.id = orders.customer_id AND orders.id = line_items.order_id "
      "AND customers.country = 'PK' AND orders.year = 2024" },
    { "Q3 (four-table, selective)",
      "SELECT customers.name, products.name FROM customers, orders, line_items, products "
      "WHERE customers.id = orders.customer_id AND orders.id = line_items.order_id "
      "AND line_items.product_id = products.id AND customers.country = 'PK' AND products.category = 'Electronics'" },
    { "Q4 (aggregation)",
      "SELECT customers.country, SUM(orders.total) FROM customers, orders "
      "WHERE customers.id = orders.customer_id AND orders.year = 2024 GROUP BY customers.country" },
    { "Q5 (adversarial)",
      "SELECT * FROM products, line_items, orders, customers WHERE products.id = line_items.product_id "
      "AND line_items.order_id = orders.id AND orders.customer_id = customers.id "
      "AND products.category = 'Electronics' AND customers.country = 'PK'" },
};
#define N_QUERIES (int)(sizeof(QUERIES) / sizeof(QUERIES[0]))

typedef struct {
    double est_cost;
    double actual_ms;
    long actual_rows;
    int skipped;
} RunResult;

static RunResult run_config(const char *sql, Catalog *cat, const char *data_dir, int config,
                             double infeasible_cost_limit) {
    RunResult r; memset(&r, 0, sizeof(r));
    char err[256];
    PlanNode *plan = parse_query(sql, err, sizeof(err));
    if (!plan) { fprintf(stderr, "parse error: %s\n", err); r.skipped = 1; return r; }
    if (bind_columns(plan, cat, err, sizeof(err)) != 0) {
        fprintf(stderr, "bind error: %s\n", err); plan_free(plan); r.skipped = 1; return r;
    }

    switch (config) {
    case 1:
        plan = rule_attach_join_conditions(plan);
        cost_estimate(plan, cat);
        break;
    case 2:
        plan = rewrite_to_fixpoint(plan);
        cost_estimate(plan, cat);
        break;
    case 3:
        plan = rule_attach_join_conditions(plan);
        cost_estimate(plan, cat);
        plan = joinorder_optimize(plan, cat, 0);
        cost_estimate(plan, cat);
        rule_join_swap(plan);
        cost_estimate(plan, cat);
        break;
    case 4:
        plan = rewrite_to_fixpoint(plan);
        cost_estimate(plan, cat);
        plan = joinorder_optimize(plan, cat, 0);
        cost_estimate(plan, cat);
        rule_join_swap(plan);
        cost_estimate(plan, cat);
        break;
    }

    r.est_cost = plan->est_cost;

    if (r.est_cost > infeasible_cost_limit) {
        r.skipped = 1;
        plan_free(plan);
        return r;
    }

    ExecConfig cfg = {0};
    double t0 = now_ms();
    Table *res = exec_plan(plan, cat, data_dir, &cfg);
    r.actual_ms = now_ms() - t0;
    r.actual_rows = res->nrows;

    table_free(res);
    plan_free(plan);
    return r;
}

int main(int argc, char **argv) {
    const char *data_dir = argc > 1 ? argv[1] : "./benchdata";
    const char *out_path = argc > 2 ? argv[2] : "./benchmark/results.txt";
    double infeasible_limit = argc > 3 ? atof(argv[3]) : 2e9;

    Catalog cat; cat.use_histograms = 0;
    if (catalog_load(&cat, data_dir) != 0) {
        fprintf(stderr, "benchmark: could not open '%s'\n", data_dir);
        return 1;
    }

    FILE *out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "benchmark: could not open '%s' for writing\n", out_path); out = stdout; }

    const char *config_names[] = { "", "no optimization", "rules only", "DP only", "full optimizer" };

    fprintf(out, "tinyopt benchmark run\n");
    fprintf(out, "dataset: %s\n", data_dir);
    fprintf(out, "tables: ");
    for (int i = 0; i < cat.n_tables; i++) fprintf(out, "%s(%ld) ", cat.tables[i].name, cat.tables[i].row_count);
    fprintf(out, "\n\n");

    double speedups[N_QUERIES];

    for (int q = 0; q < N_QUERIES; q++) {
        fprintf(out, "=== %s ===\n", QUERIES[q].label);
        fprintf(out, "%s\n\n", QUERIES[q].sql);
        fprintf(out, "%-18s %14s %14s %12s\n", "config", "est_cost", "actual_ms", "rows");

        RunResult results[5];
        for (int c = 1; c <= 4; c++) {
            results[c] = run_config(QUERIES[q].sql, &cat, data_dir, c, infeasible_limit);
            if (results[c].skipped) {
                fprintf(out, "%-18s %14.0f %14s %12s\n", config_names[c], results[c].est_cost, "n/a (too costly)", "-");
            } else {
                fprintf(out, "%-18s %14.0f %14.2f %12ld\n", config_names[c], results[c].est_cost,
                        results[c].actual_ms, results[c].actual_rows);
            }
        }

        double speedup = 0;
        if (!results[1].skipped && !results[4].skipped && results[4].actual_ms > 0)
            speedup = results[1].actual_ms / results[4].actual_ms;
        else if (results[4].est_cost > 0)
            speedup = results[1].est_cost / results[4].est_cost;
        speedups[q] = speedup;

        fprintf(out, "speedup (no-opt vs full, actual time if both ran, else cost ratio): %.1fx\n\n", speedup);
    }

    fprintf(out, "=== summary ===\n");
    double avg = 0;
    for (int q = 0; q < N_QUERIES; q++) {
        fprintf(out, "%-30s %.1fx\n", QUERIES[q].label, speedups[q]);
        avg += speedups[q];
    }
    fprintf(out, "average speedup: %.1fx\n", avg / N_QUERIES);

    if (out != stdout) fclose(out);
    printf("benchmark complete, results written to %s\n", out_path);
    return 0;
}
