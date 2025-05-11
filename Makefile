CC       = gcc
CFLAGS   = -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-alloc-size-larger-than -g -O2 -Iinclude
LDFLAGS  = -lm

SRC_DIR  = src
OBJ_DIR  = build

.PHONY: all clean

all:
	@echo "Project initialized"

clean:
	rm -rf $(OBJ_DIR) tinyopt gendata bench_driver tests/run_tests benchdata/catalog.json
