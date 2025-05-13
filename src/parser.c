#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "parser.h"

typedef enum {
    TK_IDENT, TK_NUMBER, TK_STRING, TK_STAR, TK_COMMA,
    TK_LPAREN, TK_RPAREN, TK_DOT, TK_EQ, TK_LT, TK_LE, TK_GT, TK_GE, TK_NE,
    TK_EOF
} TokKind;

typedef struct {
    TokKind kind;
    char text[128];
} Token;

#define MAX_TOKENS 1024

typedef struct {
    Token toks[MAX_TOKENS];
    int n;
    int pos;
    char err[256];
    int failed;
} Lexer;

static void lex_push(Lexer *lx, TokKind k, const char *text) {
    if (lx->n >= MAX_TOKENS) return;
    lx->toks[lx->n].kind = k;
    snprintf(lx->toks[lx->n].text, sizeof(lx->toks[lx->n].text), "%s", text);
    lx->n++;
}

static int tokenize(const char *sql, Lexer *lx) {
    lx->n = 0; lx->pos = 0; lx->failed = 0;
    const char *p = sql;
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (*p == ',') { lex_push(lx, TK_COMMA, ","); p++; continue; }
        if (*p == '(') { lex_push(lx, TK_LPAREN, "("); p++; continue; }
        if (*p == ')') { lex_push(lx, TK_RPAREN, ")"); p++; continue; }
        if (*p == '.') { lex_push(lx, TK_DOT, "."); p++; continue; }
        if (*p == '*') { lex_push(lx, TK_STAR, "*"); p++; continue; }
        if (*p == ';') { p++; continue; }
        if (*p == '=') { lex_push(lx, TK_EQ, "="); p++; continue; }
        if (*p == '<') {
            if (p[1] == '=') { lex_push(lx, TK_LE, "<="); p += 2; }
            else { lex_push(lx, TK_LT, "<"); p++; }
            continue;
        }
        if (*p == '>') {
            if (p[1] == '=') { lex_push(lx, TK_GE, ">="); p += 2; }
            else { lex_push(lx, TK_GT, ">"); p++; }
            continue;
        }
        if (*p == '!') {
            if (p[1] == '=') { lex_push(lx, TK_NE, "!="); p += 2; continue; }
            snprintf(lx->err, sizeof(lx->err), "unexpected '!' at: %s", p);
            lx->failed = 1;
            return -1;
        }
        if (*p == '\'') {
            p++;
            char buf[128]; int i = 0;
            while (*p && *p != '\'' && i < 127) buf[i++] = *p++;
            buf[i] = 0;
            if (*p == '\'') p++;
            lex_push(lx, TK_STRING, buf);
            continue;
        }
        if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
            char buf[64]; int i = 0;
            if (*p == '-') buf[i++] = *p++;
            while ((isdigit((unsigned char)*p) || *p == '.') && i < 63) buf[i++] = *p++;
            buf[i] = 0;
            lex_push(lx, TK_NUMBER, buf);
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            char buf[128]; int i = 0;
            while ((isalnum((unsigned char)*p) || *p == '_') && i < 127) buf[i++] = *p++;
            buf[i] = 0;
            lex_push(lx, TK_IDENT, buf);
            continue;
        }
        snprintf(lx->err, sizeof(lx->err), "unexpected character '%c'", *p);
        lx->failed = 1;
        return -1;
    }
    lex_push(lx, TK_EOF, "");
    return 0;
}

static Token *cur(Lexer *lx) { return &lx->toks[lx->pos]; }
static Token *peek_at(Lexer *lx, int off) {
    int i = lx->pos + off;
    if (i >= lx->n) i = lx->n - 1;
    return &lx->toks[i];
}
static void adv(Lexer *lx) { if (lx->pos < lx->n - 1) lx->pos++; }

static int kw_is(Lexer *lx, const char *kw) {
    return cur(lx)->kind == TK_IDENT && strcasecmp(cur(lx)->text, kw) == 0;
}

static int expect_kw(Lexer *lx, const char *kw) {
    if (!kw_is(lx, kw)) {
        snprintf(lx->err, sizeof(lx->err), "expected '%s' near '%s'", kw, cur(lx)->text);
        lx->failed = 1;
        return -1;
    }
    adv(lx);
    return 0;
}

static int parse_colref(Lexer *lx, ColRef *out) {
    if (cur(lx)->kind != TK_IDENT) {
        snprintf(lx->err, sizeof(lx->err), "expected column name near '%s'", cur(lx)->text);
        lx->failed = 1;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (peek_at(lx, 1)->kind == TK_DOT) {
        snprintf(out->table, NAME_LEN, "%s", cur(lx)->text);
        adv(lx); adv(lx);
        if (cur(lx)->kind != TK_IDENT) {
            snprintf(lx->err, sizeof(lx->err), "expected column name after '.'");
            lx->failed = 1;
            return -1;
        }
        snprintf(out->col, NAME_LEN, "%s", cur(lx)->text);
        adv(lx);
    } else {
        snprintf(out->col, NAME_LEN, "%s", cur(lx)->text);
        adv(lx);
    }
    return 0;
}

static AggKind agg_kind_of(const char *s) {
    if (strcasecmp(s, "SUM") == 0) return AGG_SUM;
    if (strcasecmp(s, "COUNT") == 0) return AGG_COUNT;
    if (strcasecmp(s, "AVG") == 0) return AGG_AVG;
    if (strcasecmp(s, "MIN") == 0) return AGG_MIN;
    if (strcasecmp(s, "MAX") == 0) return AGG_MAX;
    return AGG_NONE;
}

static Expr *parse_select_expr(Lexer *lx) {
    if (cur(lx)->kind == TK_IDENT) {
        AggKind ak = agg_kind_of(cur(lx)->text);
        if (ak != AGG_NONE && peek_at(lx, 1)->kind == TK_LPAREN) {
            adv(lx); adv(lx);
            Expr *e = calloc(1, sizeof(Expr));
            e->kind = EXPR_AGG;
            e->agg = ak;
            if (cur(lx)->kind == TK_STAR) {
                snprintf(e->col.table, NAME_LEN, "*");
                snprintf(e->col.col, NAME_LEN, "*");
                adv(lx);
            } else if (parse_colref(lx, &e->col) != 0) {
                free(e);
                return NULL;
            }
            if (cur(lx)->kind != TK_RPAREN) {
                ArithOp aop;
                if (strcmp(cur(lx)->text, "*") == 0 || cur(lx)->kind == TK_STAR) aop = ARITH_MUL;
                else { lx->failed = 1; snprintf(lx->err, sizeof(lx->err), "expected ) in aggregate"); free(e); return NULL; }
                adv(lx);
                ColRef rhs;
                if (parse_colref(lx, &rhs) != 0) { free(e); return NULL; }
                Expr *inner = calloc(1, sizeof(Expr));
                inner->kind = EXPR_ARITH;
                inner->arith = aop;
                Expr *l = calloc(1, sizeof(Expr)); l->kind = EXPR_COL; l->col = e->col;
                Expr *r = calloc(1, sizeof(Expr)); r->kind = EXPR_COL; r->col = rhs;
                inner->left = l; inner->right = r;
                e->kind = EXPR_AGG;
                e->left = inner;
            }
            if (cur(lx)->kind != TK_RPAREN) {
                lx->failed = 1;
                snprintf(lx->err, sizeof(lx->err), "expected ')' near '%s'", cur(lx)->text);
                free(e);
                return NULL;
            }
            adv(lx);
            snprintf(e->alias, NAME_LEN, "%s(%s.%s)", cur(lx)->text[0] ? "" : "", e->col.table, e->col.col);
            return e;
        }
        ColRef c;
        if (parse_colref(lx, &c) != 0) return NULL;
        Expr *e = calloc(1, sizeof(Expr));
        e->kind = EXPR_COL;
        e->col = c;
        if (cur(lx)->kind == TK_STAR) {
            adv(lx);
            ColRef rhs;
            if (parse_colref(lx, &rhs) != 0) { free(e); return NULL; }
            Expr *ar = calloc(1, sizeof(Expr));
            ar->kind = EXPR_ARITH;
            ar->arith = ARITH_MUL;
            ar->left = e;
            Expr *r = calloc(1, sizeof(Expr)); r->kind = EXPR_COL; r->col = rhs;
            ar->right = r;
            return ar;
        }
        return e;
    }
    if (cur(lx)->kind == TK_NUMBER) {
        Expr *e = calloc(1, sizeof(Expr));
        e->kind = EXPR_LIT;
        e->lit = value_double(atof(cur(lx)->text));
        adv(lx);
        return e;
    }
    snprintf(lx->err, sizeof(lx->err), "unexpected token '%s' in select list", cur(lx)->text);
    lx->failed = 1;
    return NULL;
}

static CmpOp tok_to_cmpop(TokKind k) {
    switch (k) {
        case TK_EQ: return OP_EQ;
        case TK_LT: return OP_LT;
        case TK_LE: return OP_LE;
        case TK_GT: return OP_GT;
        case TK_GE: return OP_GE;
        case TK_NE: return OP_NE;
        default: return OP_EQ;
    }
}

static int is_cmp_tok(TokKind k) {
    return k == TK_EQ || k == TK_LT || k == TK_LE || k == TK_GT || k == TK_GE || k == TK_NE;
}

static int parse_pred(Lexer *lx, Pred *p) {
    memset(p, 0, sizeof(*p));

    if (cur(lx)->kind == TK_NUMBER || cur(lx)->kind == TK_STRING) {
        p->lhs_is_col = 0;
        if (cur(lx)->kind == TK_STRING) p->lhs_lit = value_string(cur(lx)->text);
        else {
            const char *t = cur(lx)->text;
            p->lhs_lit = strchr(t, '.') ? value_double(atof(t)) : value_int(atoll(t));
        }
        adv(lx);
    } else {
        p->lhs_is_col = 1;
        if (parse_colref(lx, &p->lhs_col) != 0) return -1;
    }
    if (!is_cmp_tok(cur(lx)->kind)) {
        snprintf(lx->err, sizeof(lx->err), "expected comparison operator near '%s'", cur(lx)->text);
        lx->failed = 1;
        return -1;
    }
    p->op = tok_to_cmpop(cur(lx)->kind);
    adv(lx);
    if (cur(lx)->kind == TK_STRING) {
        p->rhs_is_col = 0;
        p->rhs_lit = value_string(cur(lx)->text);
        adv(lx);
    } else if (cur(lx)->kind == TK_NUMBER) {
        p->rhs_is_col = 0;
        const char *t = cur(lx)->text;
        if (strchr(t, '.')) p->rhs_lit = value_double(atof(t));
        else p->rhs_lit = value_int(atoll(t));
        adv(lx);
    } else if (cur(lx)->kind == TK_IDENT) {
        p->rhs_is_col = 1;
        if (parse_colref(lx, &p->rhs_col) != 0) return -1;
    } else {
        snprintf(lx->err, sizeof(lx->err), "expected value or column near '%s'", cur(lx)->text);
        lx->failed = 1;
        return -1;
    }
    return 0;
}

PlanNode *parse_query(const char *sql, char *errbuf, int errbuf_len) {
    Lexer lx;
    if (tokenize(sql, &lx) != 0) {
        snprintf(errbuf, errbuf_len, "%s", lx.err);
        return NULL;
    }

    if (expect_kw(&lx, "SELECT") != 0) goto fail;

    Expr *select_exprs[MAX_EXPRS];
    int n_select = 0;
    int select_star = 0;

    if (cur(&lx)->kind == TK_STAR) {
        select_star = 1;
        adv(&lx);
    } else {
        while (1) {
            Expr *e = parse_select_expr(&lx);
            if (!e) goto fail;
            if (n_select < MAX_EXPRS) select_exprs[n_select++] = e;
            if (cur(&lx)->kind == TK_COMMA) { adv(&lx); continue; }
            break;
        }
    }

    if (expect_kw(&lx, "FROM") != 0) goto fail;

    char tables[MAX_TABLES][NAME_LEN];
    int n_tables = 0;
    while (1) {
        if (cur(&lx)->kind != TK_IDENT) {
            snprintf(lx.err, sizeof(lx.err), "expected table name near '%s'", cur(&lx)->text);
            goto fail;
        }
        if (n_tables < MAX_TABLES) snprintf(tables[n_tables++], NAME_LEN, "%s", cur(&lx)->text);
        adv(&lx);
        if (cur(&lx)->kind == TK_COMMA) { adv(&lx); continue; }
        break;
    }

    Pred preds[MAX_PREDS];
    int n_preds = 0;
    if (kw_is(&lx, "WHERE")) {
        adv(&lx);
        while (1) {
            if (n_preds >= MAX_PREDS) { snprintf(lx.err, sizeof(lx.err), "too many predicates"); goto fail; }
            if (parse_pred(&lx, &preds[n_preds]) != 0) goto fail;
            n_preds++;
            if (kw_is(&lx, "AND")) { adv(&lx); continue; }
            break;
        }
    }

    int has_group_by = 0;
    ColRef group_col;
    memset(&group_col, 0, sizeof(group_col));
    if (kw_is(&lx, "GROUP")) {
        adv(&lx);
        if (expect_kw(&lx, "BY") != 0) goto fail;
        if (parse_colref(&lx, &group_col) != 0) goto fail;
        has_group_by = 1;
    }

    long limit_n = -1;
    if (kw_is(&lx, "LIMIT")) {
        adv(&lx);
        if (cur(&lx)->kind != TK_NUMBER) {
            snprintf(lx.err, sizeof(lx.err), "expected number after LIMIT");
            goto fail;
        }
        limit_n = atol(cur(&lx)->text);
        adv(&lx);
    }

    if (cur(&lx)->kind != TK_EOF) {
        snprintf(lx.err, sizeof(lx.err), "unexpected trailing input near '%s'", cur(&lx)->text);
        goto fail;
    }

    PlanNode *plan = plan_scan(tables[0]);
    for (int i = 1; i < n_tables; i++) {
        PlanNode *j = plan_new(NODE_CROSS);
        j->left = plan;
        j->right = plan_scan(tables[i]);
        plan = j;
    }

    if (n_preds > 0) {
        PlanNode *f = plan_new(NODE_FILTER);
        f->n_preds = n_preds;
        for (int i = 0; i < n_preds; i++) f->preds[i] = preds[i];
        f->left = plan;
        plan = f;
    }

    if (has_group_by) {
        PlanNode *g = plan_new(NODE_GROUPBY);
        g->group_col = group_col;
        for (int i = 0; i < n_select; i++) {
            if (select_exprs[i]->kind == EXPR_AGG) {
                g->agg_expr = expr_clone(select_exprs[i]);
                break;
            }
        }
        g->left = plan;
        plan = g;
    }

    PlanNode *proj = plan_new(NODE_PROJECT);
    if (select_star) {
        proj->n_exprs = 0;
        proj->is_star = 1;
    } else {
        proj->n_exprs = n_select;
        for (int i = 0; i < n_select; i++) proj->exprs[i] = select_exprs[i];
    }
    proj->left = plan;
    plan = proj;

    if (limit_n >= 0) {
        PlanNode *lim = plan_new(NODE_LIMIT);
        lim->limit_n = limit_n;
        lim->left = plan;
        plan = lim;
    }

    return plan;

fail:
    snprintf(errbuf, errbuf_len, "%s", lx.err);
    return NULL;
}
