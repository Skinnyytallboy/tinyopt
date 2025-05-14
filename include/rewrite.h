#ifndef TINYOPT_REWRITE_H
#define TINYOPT_REWRITE_H

#include "plan.h"

PlanNode *rewrite_to_fixpoint(PlanNode *plan);
PlanNode *rule_attach_join_conditions(PlanNode *plan);

PlanNode *rule_constant_fold(PlanNode *plan);
PlanNode *rule_predicate_pushdown(PlanNode *plan);
PlanNode *rule_projection_pushdown(PlanNode *plan);

void rule_join_swap(PlanNode *plan);

#endif
