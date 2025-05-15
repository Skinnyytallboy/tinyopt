#ifndef TINYOPT_COST_H
#define TINYOPT_COST_H

#include "plan.h"
#include "catalog.h"

void cost_estimate(PlanNode *n, Catalog *cat);
double selectivity_of(const Pred *p, Catalog *cat);
double join_condition_selectivity(const Pred *p, Catalog *cat);

#endif
