#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "value.h"

Value value_int(long long v) {
    Value r; r.type = VAL_INT; r.ival = v; r.dval = 0; r.sval = NULL;
    return r;
}

Value value_double(double v) {
    Value r; r.type = VAL_DOUBLE; r.dval = v; r.ival = 0; r.sval = NULL;
    return r;
}

Value value_string(const char *s) {
    Value r; r.type = VAL_STRING; r.ival = 0; r.dval = 0;
    r.sval = strdup(s ? s : "");
    return r;
}

Value value_copy(const Value *v) {
    if (v->type == VAL_STRING) return value_string(v->sval);
    return *v;
}

void value_free(Value *v) {
    if (v->type == VAL_STRING && v->sval) {
        free(v->sval);
        v->sval = NULL;
    }
}

double value_to_double(const Value *v) {
    switch (v->type) {
        case VAL_INT: return (double)v->ival;
        case VAL_DOUBLE: return v->dval;
        case VAL_STRING: return 0.0;
    }
    return 0.0;
}

int value_compare(const Value *a, const Value *b) {
    if (a->type == VAL_STRING || b->type == VAL_STRING) {
        const char *sa = a->type == VAL_STRING ? a->sval : "";
        const char *sb = b->type == VAL_STRING ? b->sval : "";
        return strcmp(sa, sb);
    }
    double da = value_to_double(a);
    double db = value_to_double(b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void value_print(const Value *v, char *buf, int buflen) {
    switch (v->type) {
        case VAL_INT: snprintf(buf, buflen, "%lld", v->ival); break;
        case VAL_DOUBLE: snprintf(buf, buflen, "%.2f", v->dval); break;
        case VAL_STRING: snprintf(buf, buflen, "%s", v->sval ? v->sval : ""); break;
    }
}
