/* Generates the four-table benchmark dataset described in Section 11.1
 * of the manual. Deterministic given a fixed seed (a small xorshift PRNG)
 * so the benchmark numbers in the design doc are reproducible.
 *
 * Distributions, documented here since the manual asks for it:
 *   customers.country  -- 24 country codes, skewed so a handful (incl.
 *                          'PK') are rare, most customers land in a
 *                          dozen common countries.
 *   customers.age      -- uniform 18-80.
 *   orders.year        -- 5 values (2020-2024), roughly uniform.
 *   orders.status      -- 4 statuses, skewed towards 'completed'.
 *   orders.total       -- uniform-ish 10.00-999.99.
 *   line_items.qty     -- uniform 1-10.
 *   line_items.price   -- uniform 1.00-500.00.
 *   products.category  -- 87 categories, 'Electronics' deliberately
 *                          common enough to be a meaningfully-sized slice.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long rng_state = 0x2545F4914F6CDD1DUL;

static unsigned long xorshift(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static long rnd_range(long lo, long hi) {
    return lo + (long)(xorshift() % (unsigned long)(hi - lo + 1));
}

static double rnd_double(double lo, double hi) {
    double frac = (double)(xorshift() % 1000000UL) / 1000000.0;
    return lo + frac * (hi - lo);
}

static const char *COUNTRIES[] = {
    "US","CN","IN","BR","DE","GB","FR","JP","MX","CA","AU","IT",
    "ES","KR","RU","ID","NG","EG","TR","PL","NL","SE","PK","VN"
};
#define N_COUNTRIES 24

static const char *STATUSES[] = { "completed", "completed", "completed", "pending", "cancelled", "refunded" };
#define N_STATUSES 6

static char CATEGORIES[87][32];

static void init_categories(void) {
    const char *base[] = {
        "Electronics","Books","Clothing","Home","Garden","Toys","Sports",
        "Automotive","Grocery","Beauty","Health","Office","Music","Movies",
        "Jewelry","Shoes","Baby","Pet Supplies","Tools","Software"
    };
    int nbase = sizeof(base) / sizeof(base[0]);
    for (int i = 0; i < 87; i++) {
        if (i < nbase) snprintf(CATEGORIES[i], sizeof(CATEGORIES[i]), "%s", base[i]);
        else snprintf(CATEGORIES[i], sizeof(CATEGORIES[i]), "Category%d", i);
    }
}

static const char *pick_country(void) {
    /* skewed: 60% of the time pick from the first 8 (common) countries,
     * otherwise uniformly from the full list -- so rarer codes like 'PK'
     * show up but only for a small slice of customers, which is exactly
     * what makes Q1/Q2/Q3's country filter selective. */
    if (rnd_range(0, 99) < 60) return COUNTRIES[rnd_range(0, 7)];
    return COUNTRIES[rnd_range(0, N_COUNTRIES - 1)];
}

static const char *pick_category(void) {
    /* 'Electronics' (index 0) gets a deliberate bump so Q3's join on it
     * isn't vanishingly small. */
    if (rnd_range(0, 99) < 8) return CATEGORIES[0];
    return CATEGORIES[rnd_range(0, 86)];
}

static void gen_customers(const char *dir, long n) {
    char path[600]; snprintf(path, sizeof(path), "%s/customers.csv", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "id:INT,name:STRING,country:STRING,age:INT\n");
    for (long i = 1; i <= n; i++) {
        fprintf(f, "%ld,Customer%ld,%s,%ld\n", i, i, pick_country(), rnd_range(18, 80));
    }
    fclose(f);
}

static void gen_orders(const char *dir, long n, long n_customers) {
    char path[600]; snprintf(path, sizeof(path), "%s/orders.csv", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "id:INT,customer_id:INT,total:DOUBLE,year:INT,status:STRING\n");
    for (long i = 1; i <= n; i++) {
        long cust = rnd_range(1, n_customers);
        double total = rnd_double(10.0, 999.99);
        long year = rnd_range(2020, 2024);
        fprintf(f, "%ld,%ld,%.2f,%ld,%s\n", i, cust, total, year, STATUSES[rnd_range(0, N_STATUSES - 1)]);
    }
    fclose(f);
}

static void gen_line_items(const char *dir, long n, long n_orders, long n_products) {
    char path[600]; snprintf(path, sizeof(path), "%s/line_items.csv", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "order_id:INT,product_id:INT,qty:INT,price:DOUBLE\n");
    for (long i = 0; i < n; i++) {
        long order_id = rnd_range(1, n_orders);
        long product_id = rnd_range(1, n_products);
        long qty = rnd_range(1, 10);
        double price = rnd_double(1.0, 500.0);
        fprintf(f, "%ld,%ld,%ld,%.2f\n", order_id, product_id, qty, price);
    }
    fclose(f);
}

static void gen_products(const char *dir, long n) {
    char path[600]; snprintf(path, sizeof(path), "%s/products.csv", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "id:INT,name:STRING,category:STRING,supplier_id:INT\n");
    for (long i = 1; i <= n; i++) {
        fprintf(f, "%ld,Product%ld,%s,%ld\n", i, i, pick_category(), rnd_range(1, 500));
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "./benchdata";
    double scale = argc > 2 ? atof(argv[2]) : 1.0;

    char mkcmd[700];
    snprintf(mkcmd, sizeof(mkcmd), "mkdir -p %s", dir);
    if (system(mkcmd) != 0) { /* mkdir -p failing usually just means the dir already exists */ }

    init_categories();

    long n_customers = (long)(10000 * scale);
    long n_orders = (long)(500000 * scale);
    long n_line_items = (long)(2000000 * scale);
    long n_products = (long)(50000 * scale);
    if (n_customers < 10) n_customers = 10;
    if (n_orders < 10) n_orders = 10;
    if (n_line_items < 10) n_line_items = 10;
    if (n_products < 10) n_products = 10;

    printf("generating %ld customers...\n", n_customers);
    gen_customers(dir, n_customers);
    printf("generating %ld orders...\n", n_orders);
    gen_orders(dir, n_orders, n_customers);
    printf("generating %ld line_items...\n", n_line_items);
    gen_line_items(dir, n_line_items, n_orders, n_products);
    printf("generating %ld products...\n", n_products);
    gen_products(dir, n_products);

    printf("done, wrote CSVs to %s\n", dir);
    return 0;
}
