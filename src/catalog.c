#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include "catalog.h"

typedef struct {
    double *nums;
    char **strs;
    long n, cap;
    int is_numeric;
} ColumnBuf;

static void colbuf_init(ColumnBuf *b, int numeric) {
    b->nums = NULL; b->strs = NULL; b->n = 0; b->cap = 0; b->is_numeric = numeric;
}

static void colbuf_push_num(ColumnBuf *b, double v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->nums = realloc(b->nums, b->cap * sizeof(double));
    }
    b->nums[b->n++] = v;
}

static void colbuf_push_str(ColumnBuf *b, const char *s) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 1024;
        b->strs = realloc(b->strs, b->cap * sizeof(char *));
    }
    b->strs[b->n++] = strdup(s);
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

static long distinct_count_numeric(ColumnBuf *b) {
    if (b->n == 0) return 0;
    double *tmp = malloc(b->n * sizeof(double));
    memcpy(tmp, b->nums, b->n * sizeof(double));
    qsort(tmp, b->n, sizeof(double), cmp_double);
    long d = 1;
    for (long i = 1; i < b->n; i++)
        if (tmp[i] != tmp[i - 1]) d++;
    free(tmp);
    return d;
}

static long distinct_count_string(ColumnBuf *b) {
    if (b->n == 0) return 0;
    char **tmp = malloc(b->n * sizeof(char *));
    memcpy(tmp, b->strs, b->n * sizeof(char *));
    qsort(tmp, b->n, sizeof(char *), cmp_str);
    long d = 1;
    for (long i = 1; i < b->n; i++)
        if (strcmp(tmp[i], tmp[i - 1]) != 0) d++;
    free(tmp);
    return d;
}

static void build_histogram(ColumnBuf *b, ColumnStats *cs, int n_buckets) {
    if (!b->is_numeric || b->n == 0) return;
    double *tmp = malloc(b->n * sizeof(double));
    memcpy(tmp, b->nums, b->n * sizeof(double));
    qsort(tmp, b->n, sizeof(double), cmp_double);
    cs->has_histogram = 1;
    cs->n_buckets = n_buckets;
    for (int i = 0; i <= n_buckets; i++) {
        long idx = (long)((double)i / n_buckets * (b->n - 1));
        if (idx >= b->n) idx = b->n - 1;
        cs->bucket_bounds[i] = tmp[idx];
    }
    free(tmp);
}

static ValueType parse_type_tag(const char *tag) {
    if (strcmp(tag, "INT") == 0) return VAL_INT;
    if (strcmp(tag, "DOUBLE") == 0) return VAL_DOUBLE;
    return VAL_STRING;
}

static void trim(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' ')) s[--len] = 0;
}

static int scan_one_table(TableStats *ts, const char *csv_path, int use_histograms) {
    FILE *f = fopen(csv_path, "r");
    if (!f) return -1;

    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    trim(line);

    int ncols = 0;
    char *tok = strtok(line, ",");
    while (tok && ncols < MAX_COLUMNS) {
        char *colon = strchr(tok, ':');
        ColumnStats *cs = &ts->columns[ncols];
        memset(cs, 0, sizeof(*cs));
        if (colon) {
            *colon = 0;
            snprintf(cs->name, NAME_LEN, "%s", tok);
            cs->type = parse_type_tag(colon + 1);
        } else {
            snprintf(cs->name, NAME_LEN, "%s", tok);
            cs->type = VAL_STRING;
        }
        ncols++;
        tok = strtok(NULL, ",");
    }
    ts->n_columns = ncols;

    ColumnBuf bufs[MAX_COLUMNS];
    for (int i = 0; i < ncols; i++)
        colbuf_init(&bufs[i], ts->columns[i].type != VAL_STRING);

    long rows = 0;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == 0) continue;
        int col = 0;
        char *saveptr;
        char *field = strtok_r(line, ",", &saveptr);
        while (field && col < ncols) {
            if (bufs[col].is_numeric) {
                colbuf_push_num(&bufs[col], atof(field));
            } else {
                colbuf_push_str(&bufs[col], field);
            }
            col++;
            field = strtok_r(NULL, ",", &saveptr);
        }
        rows++;
    }
    fclose(f);
    ts->row_count = rows;

    for (int i = 0; i < ncols; i++) {
        ColumnStats *cs = &ts->columns[i];
        cs->null_count = 0;
        if (bufs[i].is_numeric) {
            cs->distinct_count = distinct_count_numeric(&bufs[i]);
            double mn = 1e300, mx = -1e300;
            for (long j = 0; j < bufs[i].n; j++) {
                if (bufs[i].nums[j] < mn) mn = bufs[i].nums[j];
                if (bufs[i].nums[j] > mx) mx = bufs[i].nums[j];
            }
            if (cs->type == VAL_INT) {
                cs->min_value = value_int((long long)mn);
                cs->max_value = value_int((long long)mx);
            } else {
                cs->min_value = value_double(mn);
                cs->max_value = value_double(mx);
            }
            if (use_histograms) build_histogram(&bufs[i], cs, 32);
            free(bufs[i].nums);
        } else {
            cs->distinct_count = distinct_count_string(&bufs[i]);
            cs->min_value = value_string("");
            cs->max_value = value_string("");
            for (long j = 0; j < bufs[i].n; j++) free(bufs[i].strs[j]);
            free(bufs[i].strs);
        }
        if (cs->distinct_count < 1) cs->distinct_count = 1;
    }

    return 0;
}

static void strip_ext(const char *fname, char *out, int outlen) {
    snprintf(out, outlen, "%s", fname);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static int write_cache(Catalog *cat, const char *cache_path) {
    FILE *f = fopen(cache_path, "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"tables\": [\n");
    for (int t = 0; t < cat->n_tables; t++) {
        TableStats *ts = &cat->tables[t];
        fprintf(f, "    {\"name\": \"%s\", \"csv\": \"%s\", \"rows\": %ld, \"columns\": [\n",
                ts->name, ts->csv_path, ts->row_count);
        for (int c = 0; c < ts->n_columns; c++) {
            ColumnStats *cs = &ts->columns[c];
            char minb[128], maxb[128];
            value_print(&cs->min_value, minb, sizeof(minb));
            value_print(&cs->max_value, maxb, sizeof(maxb));
            if (minb[0] == 0) snprintf(minb, sizeof(minb), "-");
            if (maxb[0] == 0) snprintf(maxb, sizeof(maxb), "-");
            fprintf(f, "      {\"name\": \"%s\", \"type\": %d, \"distinct\": %ld, \"min\": \"%s\", \"max\": \"%s\"}%s\n",
                    cs->name, cs->type, cs->distinct_count, minb, maxb,
                    c + 1 < ts->n_columns ? "," : "");
        }
        fprintf(f, "    ]}%s\n", t + 1 < cat->n_tables ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

static int read_cache(Catalog *cat, const char *cache_path) {
    FILE *f = fopen(cache_path, "r");
    if (!f) return -1;
    char line[4096];
    int t = -1, c = -1;
    while (fgets(line, sizeof(line), f)) {
        char name[256], csvp[512];
        long rows;
        if (sscanf(line, " {\"name\": \"%255[^\"]\", \"csv\": \"%511[^\"]\", \"rows\": %ld", name, csvp, &rows) == 3) {
            t++; c = -1;
            if (t >= MAX_TABLES) break;
            snprintf(cat->tables[t].name, NAME_LEN, "%s", name);
            snprintf(cat->tables[t].csv_path, sizeof(cat->tables[t].csv_path), "%s", csvp);
            cat->tables[t].row_count = rows;
            cat->tables[t].n_columns = 0;
            continue;
        }
        char cname[256], minb[128], maxb[128];
        int type; long distinct;
        if (sscanf(line, " {\"name\": \"%255[^\"]\", \"type\": %d, \"distinct\": %ld, \"min\": \"%127[^\"]\", \"max\": \"%127[^\"]\"",
                   cname, &type, &distinct, minb, maxb) == 5) {
            if (t < 0) continue;
            c++;
            if (c >= MAX_COLUMNS) continue;
            ColumnStats *cs = &cat->tables[t].columns[c];
            memset(cs, 0, sizeof(*cs));
            snprintf(cs->name, NAME_LEN, "%s", cname);
            cs->type = (ValueType)type;
            cs->distinct_count = distinct;
            if (cs->type == VAL_INT) {
                cs->min_value = value_int(atoll(minb));
                cs->max_value = value_int(atoll(maxb));
            } else if (cs->type == VAL_DOUBLE) {
                cs->min_value = value_double(atof(minb));
                cs->max_value = value_double(atof(maxb));
            } else {
                cs->min_value = value_string(minb);
                cs->max_value = value_string(maxb);
            }
            cat->tables[t].n_columns = c + 1;
        }
    }
    fclose(f);
    cat->n_tables = t + 1;
    return cat->n_tables > 0 ? 0 : -1;
}

static long newest_csv_mtime(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    long newest = -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".csv") != 0) continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_mtime > newest) newest = st.st_mtime;
    }
    closedir(d);
    return newest;
}

int catalog_load(Catalog *cat, const char *dir) {
    memset(cat, 0, sizeof(*cat));
    char cache_path[600];
    snprintf(cache_path, sizeof(cache_path), "%s/catalog.json", dir);

    struct stat cache_st;
    long newest_csv = newest_csv_mtime(dir);
    if (stat(cache_path, &cache_st) == 0 && newest_csv >= 0 && cache_st.st_mtime >= newest_csv) {
        if (read_cache(cat, cache_path) == 0) return 0;
    }

    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".csv") != 0) continue;
        if (cat->n_tables >= MAX_TABLES) break;
        TableStats *ts = &cat->tables[cat->n_tables];
        memset(ts, 0, sizeof(*ts));
        strip_ext(de->d_name, ts->name, NAME_LEN);
        snprintf(ts->csv_path, sizeof(ts->csv_path), "%s/%s", dir, de->d_name);
        if (scan_one_table(ts, ts->csv_path, cat->use_histograms) == 0)
            cat->n_tables++;
    }
    closedir(d);

    write_cache(cat, cache_path);
    return cat->n_tables > 0 ? 0 : -1;
}

TableStats *catalog_find_table(Catalog *cat, const char *name) {
    for (int i = 0; i < cat->n_tables; i++)
        if (strcmp(cat->tables[i].name, name) == 0)
            return &cat->tables[i];
    return NULL;
}

ColumnStats *catalog_find_column(TableStats *t, const char *colname) {
    for (int i = 0; i < t->n_columns; i++)
        if (strcmp(t->columns[i].name, colname) == 0)
            return &t->columns[i];
    return NULL;
}
