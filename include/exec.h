#ifndef TINYOPT_EXEC_H
#define TINYOPT_EXEC_H

#include "plan.h"
#include "catalog.h"

#define MAX_SCHEMA_COLS 32

typedef struct {
    ColRef cols[MAX_SCHEMA_COLS];
    int ncols;
} Schema;

typedef struct {
    Schema schema;
    Value **rows;
    long nrows;
    long cap;
} Table;

typedef enum { JOIN_HASH, JOIN_SORTMERGE } JoinAlgo;

typedef struct {
    JoinAlgo join_algo;
    int trace;
} ExecConfig;

Table *table_new(void);
void table_free(Table *t);
int schema_find(const Schema *s, const char *table, const char *col);

Table *exec_plan(PlanNode *n, Catalog *cat, const char *data_dir, ExecConfig *cfg);

#endif
