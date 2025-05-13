#ifndef TINYOPT_PARSER_H
#define TINYOPT_PARSER_H

#include "plan.h"

PlanNode *parse_query(const char *sql, char *errbuf, int errbuf_len);

#endif
