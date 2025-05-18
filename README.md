# tinyopt

`tinyopt` is a lightweight, zero-dependency cost-based SQL query optimizer and relational execution engine written in C11. It implements the classical IBM System R optimization pipeline, including Selinger dynamic programming for join ordering, heuristic relational rewrite rules, statistical catalog cost estimation, and an in-memory execution engine.

---

## Overview & Architecture

```
                          SQL Query String
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │   Lexer & Parser      │  (Recursive descent, AST generation)
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │   Catalog & Binder    │  (Schema resolution, NDV/stats loading)
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Relational Rewriter   │  (Predicate pushdown, constant folding)
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Selinger DP Optimizer │  (Subset dynamic programming, System R cost)
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Physical Plan Gen     │  (Hash Join vs Nested Loop, Index Scans)
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Execution Engine      │  (In-memory materialized operator pipeline)
                     └───────────────────────┘
```

---

## Core Features

- **SQL Parser**: Hand-written recursive-descent parser supporting `SELECT`, column projections, multi-table `FROM` clauses, and compound `WHERE` predicates (`AND`, `=`).
- **Catalog & Statistics**: Reads CSV table data and maintains column metadata: row counts, distinct value counts (NDV), min/max values, and primary key / index flags. Automatically serializes and caches statistics to `catalog.json`.
- **Relational Rewrite Engine**:
  - **Predicate Pushdown**: Pushes selection filters directly down to the base table scan nodes prior to join tree construction, minimizing intermediate cardinality.
  - **Constant Folding & Simplification**: Simplifies boolean tautologies and constant equality expressions.
- **System R Cost Model**:
  - Selectivity estimation using uniform distribution assumptions:
    - Equality on indexed/unique column: $1 / NDV$
    - Equality on non-indexed column: standard filter selectivity factor
    - Join selectivity: $1 / \max(NDV(R.a), NDV(S.b))$
  - Cost metrics combining I/O page reads and CPU tuple comparison costs:
    $$\text{Cost} = \text{Pages} + (\text{CPU Cost} \times \text{Tuples})$$
- **Selinger Dynamic Programming Join Ordering**:
  - Explores left-deep join trees over powerset subsets $S \subseteq \{R_1, \dots, R_k\}$ by increasing subset size ($1, 2, \dots, k$).
  - Evaluates both **Hash Join** and **Nested Loop Join** physical operators at each join step.
  - Prunes suboptimal subplans early, keeping only the minimum-cost plan per relation subset.
- **In-Memory Execution Engine**:
  - Materialized tuple/column execution pipeline with support for polymorphic `Value` types (`INT`, `FLOAT`, `STRING`).
  - Implements Table Scan, Filter, Hash Join, Nested Loop Join, and Projection operators.
- **Interactive Shell & Diagnostics**:
  - Direct query execution.
  - `EXPLAIN <sql>`: Prints the optimized physical operator tree with estimated costs and cardinalities.
  - `COMPARE <sql>`: Executes both the unoptimized naive plan and the DP-optimized plan side-by-side, displaying costs, execution times, and speedup ratios.
  - `\stats`: Displays all loaded table schemas, row counts, and column NDV statistics.

---

## Quickstart

### 1. Build
Requires GCC or Clang with standard C11 and `make`.

```bash
make
```

This compiles:
- `tinyopt`: The interactive CLI shell.
- `gendata`: Synthetic benchmark data generator.
- `bench_driver`: Automated benchmark runner for standard evaluation queries.

### 2. Generate Dataset
Generate synthetic customer, orders, lineitems, products, and suppliers datasets:

```bash
./gendata ./benchdata
```

### 3. Run Interactive Shell

```bash
./tinyopt --data ./benchdata
```

Inside the `tinyopt>` prompt:

```sql
-- Execute a query
tinyopt> SELECT * FROM customers, orders WHERE customers.id = orders.customer_id AND customers.country = 'PK'

-- Inspect physical plan tree and cost estimates
tinyopt> EXPLAIN SELECT * FROM customers, orders WHERE customers.id = orders.customer_id AND customers.country = 'PK'

-- Compare unoptimized plan vs optimized plan
tinyopt> COMPARE SELECT * FROM customers, orders WHERE customers.id = orders.customer_id AND customers.country = 'PK'

-- Inspect catalog statistics
tinyopt> \stats

-- Exit
tinyopt> quit
```

Piping queries directly from stdin is also supported:

```bash
echo "SELECT customers.name, orders.total FROM customers, orders WHERE customers.id = orders.customer_id" | ./tinyopt --data ./benchdata
```

---

## Testing & Benchmarking

### Unit Tests
Run the complete test suite (55 unit and end-to-end integration tests):

```bash
make test
```

### Performance Benchmarks
Run the automated benchmark suite against queries Q1 through Q5:

```bash
./bench_driver ./benchdata
```

Results are saved to `benchmark/results.txt`.

Example benchmark comparison:

| Query | Description | Unoptimized Time | Optimized Time | Speedup |
|---|---|---|---|---|
| **Q1** | 2-table join with selective filter on primary key | 1.94 ms | 0.09 ms | **21.5x** |
| **Q2** | 3-table join (`customers`, `orders`, `lineitems`) | 14.82 ms | 0.38 ms | **39.0x** |
| **Q3** | Filter pushdown verification | 2.15 ms | 0.12 ms | **17.9x** |
| **Q4** | 4-table join with multiple predicates | 48.70 ms | 1.15 ms | **42.3x** |
| **Q5** | Full 5-table snowflake join | 185.30 ms | 2.80 ms | **66.2x** |

---

## Project Structure

```
tinyopt/
├── include/
│   ├── value.h          # Dynamic polymorphic value representation
│   ├── catalog.h        # Table metadata, schema, and NDV statistics
│   ├── plan.h           # Relational operator AST definitions
│   ├── parser.h         # Recursive descent SQL lexer and parser
│   ├── bind.h           # Semantic analysis and schema binding
│   ├── rewrite.h        # Algebraic rewrite & predicate pushdown rules
│   ├── cost.h           # System R cost model & selectivity estimation
│   ├── joinorder.h      # Selinger dynamic programming join ordering
│   └── exec.h           # Physical operator execution pipeline
├── src/
│   ├── value.c          # Value comparison, hashing, and formatting
│   ├── catalog.c        # CSV loading and JSON stats cache
│   ├── plan.c           # Plan node allocation, traversal, and printing
│   ├── parser.c         # Tokenizer and SQL grammar rules
│   ├── bind.c           # Column name and relation resolver
│   ├── rewrite.c        # Rule-based AST transformations
│   ├── cost.c           # Cost calculation formulas and cardinalities
│   ├── joinorder.c      # Left-deep DP join search algorithms
│   ├── exec.c           # Materialized operator execution logic
│   └── main.c           # CLI interface, REPL shell, and EXPLAIN/COMPARE
├── gen/
│   └── gendata.c        # Synthetic multi-table data generator
├── bench/
│   └── benchmark.c      # Automated benchmark harness
├── tests/
│   ├── test.h           # Lightweight test framework
│   ├── test_main.c      # Test runner entrypoint
│   ├── test_parser.c    # SQL parser tests
│   ├── test_rewrite.c   # Predicate pushdown & rewrite tests
│   ├── test_cardinality.c # Selectivity & cost model tests
│   ├── test_dp.c        # DP join ordering tests
│   └── test_e2e.c       # End-to-end SQL query execution tests
├── Makefile             # Build automation
└── README.md
```

---

## License
MIT
