CC       = gcc
CFLAGS   = -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-alloc-size-larger-than -g -O2 -Iinclude
LDFLAGS  = -lm

SRC_DIR  = src
OBJ_DIR  = build

CORE_SRCS = $(SRC_DIR)/value.c $(SRC_DIR)/catalog.c $(SRC_DIR)/plan.c \
            $(SRC_DIR)/parser.c $(SRC_DIR)/bind.c $(SRC_DIR)/rewrite.c \
            $(SRC_DIR)/cost.c $(SRC_DIR)/joinorder.c $(SRC_DIR)/exec.c
CORE_OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))

HEADERS = $(wildcard include/*.h)

.PHONY: all clean test

all: tinyopt gendata bench_driver

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

tinyopt: $(CORE_OBJS) $(OBJ_DIR)/main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

gendata: gen/gendata.c
	$(CC) $(CFLAGS) -o $@ gen/gendata.c $(LDFLAGS)

bench_driver: $(CORE_OBJS) bench/benchmark.c
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) bench/benchmark.c $(LDFLAGS)

test: tests/run_tests
	./tests/run_tests

tests/run_tests: $(CORE_OBJS) tests/test_main.c tests/test_parser.c tests/test_rewrite.c \
                  tests/test_cardinality.c tests/test_dp.c tests/test_e2e.c
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) tests/test_main.c tests/test_parser.c \
	    tests/test_rewrite.c tests/test_cardinality.c tests/test_dp.c tests/test_e2e.c $(LDFLAGS)

clean:
	rm -rf $(OBJ_DIR) tinyopt gendata bench_driver tests/run_tests benchdata/catalog.json
