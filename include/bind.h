#ifndef TINYOPT_BIND_H
#define TINYOPT_BIND_H

#include "plan.h"
#include "catalog.h"

int bind_columns(PlanNode *plan, Catalog *cat, char *errbuf, int errbuf_len);

#endif
