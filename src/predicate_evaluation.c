#include "database_engine.h"

THREAD_LOCAL const Scope* g_outer_scope = NULL;

bool parse_column_reference(Parser* p, ColumnName* name) {
    memset(name, 0, sizeof(*name));
    if (!parse_identifier(p, name->column)) return false;
    if (consume_token(p, TK_DOT)) {
        memcpy(name->table, name->column, sizeof(name->table));
        if (!parse_identifier(p, name->column)) return false;
    }
    return true;
}

bool resolve_column_reference(const Scope* scope, const ColumnName* name, Binding* binding) {
    if (!strcmp(name->column, "ROWNUM")) {
        binding->table = 0;
        binding->column = 0;
        return true;
    }
    int found = 0;
    for (int t = 0; t < scope->count; t++) {
        if (!scope->tables[t]) continue;
        if (name->table[0]) {
            bool matches_name = !strcmp(name->table, scope->tables[t]->name);
            bool matches_alias = scope->aliases[t][0] && !strcmp(name->table, scope->aliases[t]);
            if (!matches_name && !matches_alias) continue;
        }
        int c = find_column_index(scope->tables[t], name->column);
        if (c >= 0) { binding->table = t; binding->column = c; found++; }
    }
    if (found == 1) return true;
    if (found == 0 && g_outer_scope) {
        for (int t = 0; t < g_outer_scope->count; t++) {
            if (!g_outer_scope->tables[t]) continue;
            if (name->table[0]) {
                bool matches_name = !strcmp(name->table, g_outer_scope->tables[t]->name);
                bool matches_alias = g_outer_scope->aliases[t][0] && !strcmp(name->table, g_outer_scope->aliases[t]);
                if (!matches_name && !matches_alias) continue;
            }
            int c = find_column_index(g_outer_scope->tables[t], name->column);
            if (c >= 0) {
                binding->table = t | 0x1000;
                binding->column = c;
                found++;
            }
        }
    }
    return found == 1;
}

static bool match_like(const char* pat, const char* str) {
    const char* wildcard = NULL;
    const char* retry = NULL;
    while (*str) {
        if (*pat == '%') {
            wildcard = ++pat;
            retry = str;
        } else if (*pat == '_' || *pat == *str) {
            pat++;
            str++;
        } else if (wildcard) {
            pat = wildcard;
            str = ++retry;
        } else return false;
    }
    while (*pat == '%') pat++;
    return *pat == '\0';
}

static bool match_ilike(const char* pat, const char* str) {
    const char* wildcard = NULL;
    const char* retry = NULL;
    while (*str) {
        if (*pat == '%') {
            wildcard = ++pat;
            retry = str;
        } else if (*pat == '_' || uppercase_ascii(*pat) == uppercase_ascii(*str)) {
            pat++;
            str++;
        } else if (wildcard) {
            pat = wildcard;
            str = ++retry;
        } else return false;
    }
    while (*pat == '%') pat++;
    return *pat == '\0';
}

bool parse_comparison_predicate(Parser* p, const Scope* scope, Predicate* pred) {
    memset(pred, 0, sizeof(*pred));
    bool not_exists = false;
    if (consume_keyword(p, "NOT")) {
        not_exists = true;
    }
    if (consume_keyword(p, "EXISTS")) {
        pred->op = not_exists ? PRED_NOT_EXISTS : PRED_EXISTS;
        if (p->kind != TK_LP) return false;
        const char* sub_start = p->cursor;
        int depth = 1;
        const char* s = p->cursor;
        bool in_q = false;
        while (*s && depth > 0) {
            if (*s == '\'') in_q = !in_q;
            else if (!in_q) {
                if (*s == '(') depth++;
                else if (*s == ')') depth--;
            }
            if (depth == 0) break;
            s++;
        }
        if (depth != 0) return false;
        size_t sub_len = (size_t)(s - sub_start);
        if (sub_len >= sizeof(pred->subquery_sql)) return false;
        memcpy(pred->subquery_sql, sub_start, sub_len);
        pred->subquery_sql[sub_len] = '\0';
        p->cursor = s + 1;
        scan_next_token(p);
        return true;
    }
    if (not_exists) return false;
    ColumnName name;
    bool func_cosine = consume_keyword(p, "COSINE_DISTANCE");
    bool func_l2 = !func_cosine && consume_keyword(p, "L2_DISTANCE");
    if (func_cosine || func_l2) {
        if (!consume_token(p, TK_LP)) return false;
        if (!parse_column_reference(p, &name) || !resolve_column_reference(scope, &name, &pred->left)) return false;
        if (!consume_token(p, TK_COMMA) || p->kind != TK_STRING) return false;
        pred->expr_op = func_cosine ? 'C' : 'L';
        snprintf(pred->expr_operand.text, sizeof(pred->expr_operand.text), "%s", p->text);
        pred->expr_operand.is_null = false;
        scan_next_token(p);
        if (!consume_token(p, TK_RP)) return false;
    } else {
        if (!parse_column_reference(p, &name) || !resolve_column_reference(scope, &name, &pred->left)) return false;
    }
    if (!strcmp(name.column, "ROWNUM")) {
        pred->is_rownum = true;
    }
    bool outer_binding = (pred->left.table & 0x1000) != 0;
    const Scope* left_scope = outer_binding ? g_outer_scope : scope;
    int left_table = pred->left.table & ~0x1000;
    if (!left_scope || left_table < 0 || left_table >= left_scope->count) return false;
    const ColumnDef* left = &left_scope->tables[left_table]->columns[pred->left.column];
    ColumnDef num_col_def = { .type = TYPE_NUMBER, .size = 40, .nullable = true };

    if (pred->expr_op == 0 && (p->kind == TK_COSINE_DIST || p->kind == TK_L2_DIST)) {
        pred->expr_op = (p->kind == TK_COSINE_DIST) ? 'C' : 'L';
        scan_next_token(p);
        if (p->kind != TK_STRING) return false;
        snprintf(pred->expr_operand.text, sizeof(pred->expr_operand.text), "%s", p->text);
        pred->expr_operand.is_null = false;
        scan_next_token(p);
    } else if (pred->expr_op == 0 && (p->kind == TK_PLUS || p->kind == TK_MINUS || p->kind == TK_STAR || p->kind == TK_SLASH)) {
        pred->expr_op = (p->kind == TK_PLUS ? '+' : p->kind == TK_MINUS ? '-' : p->kind == TK_STAR ? '*' : '/');
        scan_next_token(p);
        if (!parse_typed_literal(p, left, &pred->expr_operand)) return false;
    }
    if (pred->expr_op == 'C' || pred->expr_op == 'L') {
        left = &num_col_def;
    }

    if (consume_keyword(p, "IS")) {
        pred->is_null_test = true;
        pred->negate = consume_keyword(p, "NOT");
        return consume_keyword(p, "NULL");
    }
    bool neg = consume_keyword(p, "NOT");
    if (consume_keyword(p, "LIKE")) {
        pred->op = PRED_LIKE;
        pred->negate = neg;
        if (p->kind != TK_STRING) return false;
        if (strlen(p->text) >= sizeof(pred->value.text)) return false;
        snprintf(pred->value.text, sizeof(pred->value.text), "%s", p->text);
        scan_next_token(p);
        return true;
    }
    if (consume_keyword(p, "ILIKE")) {
        pred->op = PRED_ILIKE;
        pred->negate = neg;
        if (p->kind != TK_STRING) return false;
        if (strlen(p->text) >= sizeof(pred->value.text)) return false;
        snprintf(pred->value.text, sizeof(pred->value.text), "%s", p->text);
        scan_next_token(p);
        return true;
    }
    if (consume_keyword(p, "BETWEEN")) {
        pred->op = PRED_BETWEEN;
        pred->negate = neg;
        if (!parse_typed_literal(p, left, &pred->value) ||
            !consume_keyword(p, "AND") ||
            !parse_typed_literal(p, left, &pred->value2)) return false;
        return true;
    }
    if (consume_keyword(p, "IN")) {
        pred->op = neg ? PRED_NOT_IN : PRED_IN;
        if (!consume_token(p, TK_LP)) return false;
        if (consume_keyword(p, "SELECT")) {
            QueryResultCapture capture = {0};
            capture.silent = true;
            QueryResultCapture* old_cap = g_result_capture;
            g_result_capture = &capture;
            bool sub_ok = execute_select_query(p);
            g_result_capture = old_cap;
            if (!sub_ok || !consume_token(p, TK_RP)) { free(capture.rows); return false; }
            if (capture.row_count > 16) {
                free(capture.rows);
                return report_database_error("IN subquery exceeds the 16-value limit");
            }
            for (int r = 0; r < capture.row_count; r++) {
                pred->in_values[pred->in_count++] = capture.rows[r][0];
            }
            free(capture.rows);
            return true;
        }
        do {
            if (pred->in_count == 16 || !parse_typed_literal(p, left, &pred->in_values[pred->in_count++])) return false;
        } while (consume_token(p, TK_COMMA));
        return consume_token(p, TK_RP);
    }
    if (neg) return false;
    pred->op = (int)p->kind;
    if (p->kind < TK_EQ || p->kind > TK_GE) return false;
    scan_next_token(p);
    if (p->kind == TK_ID && !token_is_keyword(p, "NULL")) {
        if (!parse_column_reference(p, &name) || !resolve_column_reference(scope, &name, &pred->right)) return false;
        pred->right_column = true;
        DataType rtype = (pred->right.table & 0x1000) ?
            g_outer_scope->tables[pred->right.table & ~0x1000]->columns[pred->right.column].type :
            scope->tables[pred->right.table]->columns[pred->right.column].type;
        return left->type == rtype;
    }
    return parse_typed_literal(p, left, &pred->value);
}

static int alloc_node(Filter* f, FilterNodeType type) {
    if (f->node_count >= 64) return -1;
    int idx = f->node_count++;
    f->nodes[idx].type = type;
    f->nodes[idx].left_child = -1;
    f->nodes[idx].right_child = -1;
    return idx;
}

static int parse_or_expr(Parser* p, const Scope* scope, Filter* filter, unsigned depth);

static int parse_unary_expr(Parser* p, const Scope* scope, Filter* filter, unsigned depth) {
    if (consume_token(p, TK_LP)) {
        if (depth >= 64) {
            report_database_error("WHERE nesting limit exceeded");
            return -1;
        }
        int inner = parse_or_expr(p, scope, filter, depth + 1);
        if (inner < 0 || !consume_token(p, TK_RP)) return -1;
        return inner;
    }
    int idx = alloc_node(filter, FILTER_NODE_PRED);
    if (idx < 0) return -1;
    if (!parse_comparison_predicate(p, scope, &filter->nodes[idx].pred)) return -1;
    return idx;
}

static int parse_and_expr(Parser* p, const Scope* scope, Filter* filter, unsigned depth) {
    int left = parse_unary_expr(p, scope, filter, depth);
    if (left < 0) return -1;
    while (consume_keyword(p, "AND")) {
        int right = parse_unary_expr(p, scope, filter, depth);
        if (right < 0) return -1;
        int parent = alloc_node(filter, FILTER_NODE_AND);
        if (parent < 0) return -1;
        filter->nodes[parent].left_child = left;
        filter->nodes[parent].right_child = right;
        left = parent;
    }
    return left;
}

static int parse_or_expr(Parser* p, const Scope* scope, Filter* filter, unsigned depth) {
    int left = parse_and_expr(p, scope, filter, depth);
    if (left < 0) return -1;
    while (consume_keyword(p, "OR")) {
        int right = parse_and_expr(p, scope, filter, depth);
        if (right < 0) return -1;
        int parent = alloc_node(filter, FILTER_NODE_OR);
        if (parent < 0) return -1;
        filter->nodes[parent].left_child = left;
        filter->nodes[parent].right_child = right;
        left = parent;
    }
    return left;
}

bool parse_where_clause(Parser* p, const Scope* scope, Filter* filter) {
    memset(filter, 0, sizeof(*filter));
    filter->root = -1;
    if (!consume_keyword(p, "WHERE")) return true;
    filter->root = parse_or_expr(p, scope, filter, 0);
    return filter->root >= 0;
}

bool evaluate_comparison_predicate(const Scope* scope, const Predicate* pred) {
    if (pred->op == PRED_EXISTS || pred->op == PRED_NOT_EXISTS) {
        Parser sub_p = {0};
        sub_p.cursor = pred->subquery_sql;
        scan_next_token(&sub_p);
        if (!consume_keyword(&sub_p, "SELECT")) return false;

        QueryResultCapture capture = {0};
        capture.silent = true;
        QueryResultCapture* old_cap = g_result_capture;
        const Scope* old_outer = g_outer_scope;
        g_result_capture = &capture;
        g_outer_scope = scope;
        bool sub_ok = execute_select_query(&sub_p);
        g_outer_scope = old_outer;
        g_result_capture = old_cap;

        bool has_rows = sub_ok && capture.row_count > 0;
        free(capture.rows);
        /* A failed subquery is an error, not "no rows": NOT EXISTS must not
           silently evaluate to true for every row. */
        if (!sub_ok) return false;
        return (pred->op == PRED_EXISTS) ? has_rows : !has_rows;
    }
    static const Value null_val = { .is_null = true, .text = "" };
    const Value* left = &null_val;
    if (pred->left.table & 0x1000) {
        int t = pred->left.table & ~0x1000;
        if (g_outer_scope && t < g_outer_scope->count && g_outer_scope->rows[t])
            left = &g_outer_scope->rows[t]->values[pred->left.column];
    } else if (pred->left.table < scope->count && scope->rows[pred->left.table]) {
        left = &scope->rows[pred->left.table]->values[pred->left.column];
    }

    Value eval_left;
    if (pred->expr_op != 0 && !left->is_null && !pred->expr_operand.is_null) {
        long double res = 0;
        if (pred->expr_op == 'C') {
            res = (long double)vector_cosine_distance(left->text, pred->expr_operand.text);
        } else if (pred->expr_op == 'L') {
            res = (long double)vector_l2_distance(left->text, pred->expr_operand.text);
        } else {
            long double v = strtold(left->text, NULL);
            long double op = strtold(pred->expr_operand.text, NULL);
            switch (pred->expr_op) {
            case '+': res = v + op; break;
            case '-': res = v - op; break;
            case '*': res = v * op; break;
            case '/': res = (op != 0) ? (v / op) : 0; break;
            }
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "%.6Lf", res);
        char* dot = strchr(buf, '.');
        if (dot) {
            char* end = buf + strlen(buf) - 1;
            while (end > dot && *end == '0') *end-- = '\0';
            if (end == dot) *end = '\0';
        }
        memset(&eval_left, 0, sizeof(eval_left));
        if (!normalize_decimal(buf, eval_left.text)) snprintf(eval_left.text, sizeof(eval_left.text), "%s", buf);
        left = &eval_left;
    }

    if (pred->is_rownum) return true;
    if (pred->is_null_test) return pred->negate ? !left->is_null : left->is_null;
    const Value* right = &null_val;
    if (pred->right_column) {
        if (pred->right.table & 0x1000) {
            int t = pred->right.table & ~0x1000;
            if (g_outer_scope && t < g_outer_scope->count && g_outer_scope->rows[t])
                right = &g_outer_scope->rows[t]->values[pred->right.column];
        } else if (pred->right.table < scope->count && scope->rows[pred->right.table]) {
            right = &scope->rows[pred->right.table]->values[pred->right.column];
        }
    } else {
        right = &pred->value;
    }
    DataType type = (pred->expr_op == 'C' || pred->expr_op == 'L') ? TYPE_NUMBER :
        ((pred->left.table & 0x1000) ?
        ((g_outer_scope && (pred->left.table & ~0x1000) < g_outer_scope->count && g_outer_scope->tables[pred->left.table & ~0x1000]) ?
            g_outer_scope->tables[pred->left.table & ~0x1000]->columns[pred->left.column].type : TYPE_NUMBER) :
        ((pred->left.table < scope->count && scope->tables[pred->left.table]) ?
            scope->tables[pred->left.table]->columns[pred->left.column].type : TYPE_NUMBER));
    if (pred->op == PRED_LIKE || pred->op == PRED_ILIKE) {
        if (left->is_null) return false;
        bool match = (pred->op == PRED_ILIKE) ? match_ilike(pred->value.text, left->text)
                                              : match_like(pred->value.text, left->text);
        return pred->negate ? !match : match;
    }
    if (pred->op == PRED_BETWEEN) {
        if (left->is_null || pred->value.is_null || pred->value2.is_null) return false;
        int c1 = compare_typed_values(type, left, &pred->value);
        int c2 = compare_typed_values(type, left, &pred->value2);
        bool match = (c1 >= 0 && c2 <= 0);
        return pred->negate ? !match : match;
    }
    if (pred->op == PRED_IN || pred->op == PRED_NOT_IN) {
        if (left->is_null) return false;
        bool found = false, any_null = false;
        for (int i = 0; i < pred->in_count; i++) {
            if (pred->in_values[i].is_null) { any_null = true; continue; }
            if (compare_typed_values(type, left, &pred->in_values[i]) == 0) {
                found = true;
                break;
            }
        }
        /* SQL three-valued logic: NOT IN with a NULL in the list and no match
           is UNKNOWN, which WHERE treats as not-true. */
        if (pred->op == PRED_NOT_IN && !found && any_null) return false;
        return pred->op == PRED_IN ? found : !found;
    }
    if (left->is_null || right->is_null) return false;
    int cmp = compare_typed_values(type, left, right);
    switch (pred->op) {
    case TK_EQ: return cmp == 0; case TK_NE: return cmp != 0;
    case TK_LT: return cmp < 0; case TK_LE: return cmp <= 0;
    case TK_GT: return cmp > 0; case TK_GE: return cmp >= 0;
    default: return false;
    }
}

static bool eval_node(const Scope* scope, const Filter* f, int idx, int depth) {
    if (idx < 0 || idx >= f->node_count || depth > 128) return true;
    const FilterNode* n = &f->nodes[idx];
    switch (n->type) {
    case FILTER_NODE_PRED:
        return evaluate_comparison_predicate(scope, &n->pred);
    case FILTER_NODE_AND:
        return eval_node(scope, f, n->left_child, depth + 1) && eval_node(scope, f, n->right_child, depth + 1);
    case FILTER_NODE_OR:
        return eval_node(scope, f, n->left_child, depth + 1) || eval_node(scope, f, n->right_child, depth + 1);
    case FILTER_NODE_NOT:
        return !eval_node(scope, f, n->left_child, depth + 1);
    default:
        return false;
    }
}

bool evaluate_where_clause(const Scope* scope, const Filter* filter) {
    if (filter->node_count == 0 || filter->root < 0) return true;
    return eval_node(scope, filter, filter->root, 0);
}
