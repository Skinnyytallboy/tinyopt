#ifndef TINYOPT_JOINORDER_H
#define TINYOPT_JOINORDER_H

#include "plan.h"
#include "catalog.h"

PlanNode *joinorder_optimize(PlanNode *root, Catalog *cat, int allow_bushy);

#endif
