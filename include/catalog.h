#ifndef TINYOPT_CATALOG_H
#define TINYOPT_CATALOG_H

#include "value.h"

#define MAX_COLUMNS 32

typedef struct {
    char name[NAME_LEN];
    ValueType type;
    long distinct_count;
    Value min_value;
    Value max_value;
    long null_count;

    int has_histogram;
    int n_buckets;
    double bucket_bounds[33];
} ColumnStats;

typedef struct {
    char name[NAME_LEN];
    char csv_path[512];
    long row_count;
    int n_columns;
    ColumnStats columns[MAX_COLUMNS];
} TableStats;

typedef struct {
    int n_tables;
    TableStats tables[MAX_TABLES];
    int use_histograms;
} Catalog;

int catalog_load(Catalog *cat, const char *dir);

TableStats *catalog_find_table(Catalog *cat, const char *name);
ColumnStats *catalog_find_column(TableStats *t, const char *colname);

#endif
