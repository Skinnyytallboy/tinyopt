#ifndef TINYOPT_VALUE_H
#define TINYOPT_VALUE_H

#define NAME_LEN 64
#define MAX_TABLES 16

typedef enum {
    VAL_INT,
    VAL_DOUBLE,
    VAL_STRING
} ValueType;

typedef struct {
    ValueType type;
    long long ival;
    double dval;
    char *sval;
} Value;

Value value_int(long long v);
Value value_double(double v);
Value value_string(const char *s);
Value value_copy(const Value *v);
void value_free(Value *v);

int value_compare(const Value *a, const Value *b);
double value_to_double(const Value *v);
void value_print(const Value *v, char *buf, int buflen);

#endif
