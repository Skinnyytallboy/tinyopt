#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include "catalog.h"
#include "parser.h"
#include "bind.h"
#include "rewrite.h"
#include "cost.h"
#include "joinorder.h"
#include "exec.h"

typedef struct {
    long n_queries;
    double sum_speedup;
    long n_compares;
    double sum_plan_ms;
    double sum_exec_ms;
} SessionStats;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static PlanNode *optimize_query(const char *sql, Catalog *cat, int bushy, char *err, int errlen) {
    PlanNode *plan = parse_query(sql, err, errlen);
    if (!plan) return NULL;
    if (bind_columns(plan, cat, err, errlen) != 0) { plan_free(plan); return NULL; }
    plan = rewrite_to_fixpoint(plan);
    cost_estimate(plan, cat);
    plan = joinorder_optimize(plan, cat, bushy);
    cost_estimate(plan, cat);
    rule_join_swap(plan);
    cost_estimate(plan, cat);
    return plan;
}

static PlanNode *naive_query(const char *sql, Catalog *cat, char *err, int errlen) {
    PlanNode *plan = parse_query(sql, err, errlen);
    if (!plan) return NULL;
    if (bind_columns(plan, cat, err, errlen) != 0) { plan_free(plan); return NULL; }
    plan = rule_attach_join_conditions(plan);
    cost_estimate(plan, cat);
    return plan;
}

static void print_rows(Table *t, long max_rows) {
    for (int i = 0; i < t->schema.ncols; i++)
        printf("%s%s.%s", i ? " | " : "", t->schema.cols[i].table, t->schema.cols[i].col);
    printf("\n");
    long shown = t->nrows < max_rows ? t->nrows : max_rows;
    for (long i = 0; i < shown; i++) {
        for (int c = 0; c < t->schema.ncols; c++) {
            char buf[128];
            value_print(&t->rows[i][c], buf, sizeof(buf));
            printf("%s%s", c ? " | " : "", buf);
        }
        printf("\n");
    }
    if (t->nrows > shown) printf("... (%ld more rows)\n", t->nrows - shown);
}

static void run_explain(const char *sql, Catalog *cat, int bushy) {
    char err[256];
    PlanNode *plan = optimize_query(sql, cat, bushy, err, sizeof(err));
    if (!plan) { printf("error: %s\n", err); return; }
    printf("plan:\n");
    plan_print(plan, 1, 1);
    printf("estimated cost: %.0f\n", plan->est_cost);
    plan_free(plan);
}

static void run_compare(const char *sql, Catalog *cat, const char *data_dir, int bushy, SessionStats *st) {
    char err[256];

    PlanNode *naive = naive_query(sql, cat, err, sizeof(err));
    if (!naive) { printf("error: %s\n", err); return; }
    ExecConfig cfg = {0};
    double t0 = now_ms();
    Table *naive_res = exec_plan(naive, cat, data_dir, &cfg);
    double naive_time = now_ms() - t0;

    printf("--- WITHOUT optimizer (FROM order, no rewrites) ---\n");
    plan_print(naive, 1, 1);
    printf("estimated cost: %.0f\n", naive->est_cost);
    printf("actual time: %.3f sec, %ld result rows\n\n", naive_time / 1000.0, naive_res->nrows);

    PlanNode *opt = optimize_query(sql, cat, bushy, err, sizeof(err));
    if (!opt) { printf("error: %s\n", err); plan_free(naive); table_free(naive_res); return; }
    t0 = now_ms();
    Table *opt_res = exec_plan(opt, cat, data_dir, &cfg);
    double opt_time = now_ms() - t0;

    printf("--- WITH optimizer (rules + cost-based DP join ordering) ---\n");
    plan_print(opt, 1, 1);
    printf("estimated cost: %.0f\n", opt->est_cost);
    printf("actual time: %.3f sec, %ld result rows\n\n", opt_time / 1000.0, opt_res->nrows);

    double speedup = opt_time > 0 ? naive_time / opt_time : 0;
    double cost_ratio = opt->est_cost > 0 ? naive->est_cost / opt->est_cost : 0;
    printf("speedup: %.1fx\n", speedup);
    printf("plan cost ratio: %.0fx\n", cost_ratio);

    if (naive_res->nrows != opt_res->nrows) {
        printf("WARNING: row count mismatch (%ld vs %ld) -- the optimized plan changed results!\n",
               naive_res->nrows, opt_res->nrows);
    }

    st->n_queries++;
    st->n_compares++;
    st->sum_speedup += speedup;

    plan_free(naive); table_free(naive_res);
    plan_free(opt); table_free(opt_res);
}

static void run_query(const char *sql, Catalog *cat, const char *data_dir, int bushy, SessionStats *st) {
    char err[256];
    double t0 = now_ms();
    PlanNode *plan = optimize_query(sql, cat, bushy, err, sizeof(err));
    if (!plan) { printf("error: %s\n", err); return; }
    double plan_ms = now_ms() - t0;

    ExecConfig cfg = {0};
    t0 = now_ms();
    Table *res = exec_plan(plan, cat, data_dir, &cfg);
    double exec_ms = now_ms() - t0;

    print_rows(res, 20);
    printf("(%ld rows, plan %.2fms, exec %.2fms)\n", res->nrows, plan_ms, exec_ms);

    st->n_queries++;
    st->sum_plan_ms += plan_ms;
    st->sum_exec_ms += exec_ms;

    plan_free(plan);
    table_free(res);
}

static void run_trace(const char *sql, Catalog *cat, const char *data_dir, int bushy) {
    char err[256];
    PlanNode *plan = optimize_query(sql, cat, bushy, err, sizeof(err));
    if (!plan) { printf("error: %s\n", err); return; }
    printf("plan (estimated):\n");
    plan_print(plan, 1, 1);
    ExecConfig cfg = {0};
    cfg.trace = 1;
    printf("actual (bottom-up):\n");
    Table *res = exec_plan(plan, cat, data_dir, &cfg);
    table_free(res);
    plan_free(plan);
}

static void print_stats(SessionStats *st) {
    printf("queries executed:    %ld\n", st->n_queries);
    if (st->n_compares > 0)
        printf("optimizer wins:      %ld (avg speedup %.1fx)\n", st->n_compares,
               st->sum_speedup / st->n_compares);
    long plain = st->n_queries - st->n_compares;
    if (plain > 0) {
        printf("average plan time:   %.2fms\n", st->sum_plan_ms / plain);
        printf("average exec time:   %.2fms\n", st->sum_exec_ms / plain);
    }
}

static void print_catalog_summary(Catalog *cat) {
    printf("tinyopt: opened catalog with %d tables\n", cat->n_tables);
    for (int i = 0; i < cat->n_tables; i++)
        printf("  %-14s %8ld rows, %d columns\n", cat->tables[i].name,
               cat->tables[i].row_count, cat->tables[i].n_columns);
}

static int starts_with_ci(const char *s, const char *prefix) {
    return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

int main(int argc, char **argv) {
    char data_dir[512] = "./benchdata";
    int bushy = 0;
    const char *join_algo_name = "hash";
    int histograms = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) snprintf(data_dir, sizeof(data_dir), "%s", argv[++i]);
        else if (strcmp(argv[i], "--bushy") == 0) bushy = 1;
        else if (strcmp(argv[i], "--histograms") == 0) histograms = 1;
        else if (strncmp(argv[i], "--join-algo=", 12) == 0) join_algo_name = argv[i] + 12;
    }

    Catalog cat;
    cat.use_histograms = histograms;
    if (catalog_load(&cat, data_dir) != 0) {
        fprintf(stderr, "tinyopt: could not open any .csv files in '%s'\n", data_dir);
        return 1;
    }
    print_catalog_summary(&cat);

    ExecConfig global_cfg = {0};
    global_cfg.join_algo = strcasecmp(join_algo_name, "merge") == 0 ? JOIN_SORTMERGE : JOIN_HASH;
    (void)global_cfg;

    SessionStats stats; memset(&stats, 0, sizeof(stats));

    char line[8192];
    int interactive = isatty(fileno(stdin));
    if (interactive) printf("tinyopt> ");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) { if (interactive) { printf("tinyopt> "); fflush(stdout); } continue; }

        if (strcmp(line, "\\stats") == 0) {
            print_stats(&stats);
        } else if (starts_with_ci(line, "quit") || starts_with_ci(line, "exit")) {
            break;
        } else if (starts_with_ci(line, "LOAD ")) {
            snprintf(data_dir, sizeof(data_dir), "%s", line + 5);
            if (catalog_load(&cat, data_dir) == 0) print_catalog_summary(&cat);
            else fprintf(stderr, "tinyopt: could not load '%s'\n", data_dir);
        } else if (starts_with_ci(line, "EXPLAIN ")) {
            run_explain(line + 8, &cat, bushy);
        } else if (starts_with_ci(line, "COMPARE ")) {
            run_compare(line + 8, &cat, data_dir, bushy, &stats);
        } else if (starts_with_ci(line, "TRACE ")) {
            run_trace(line + 6, &cat, data_dir, bushy);
        } else {
            run_query(line, &cat, data_dir, bushy, &stats);
        }

        if (interactive) { printf("tinyopt> "); fflush(stdout); }
    }
    if (interactive) printf("\n");
    return 0;
}
