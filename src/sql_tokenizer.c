#include "database_engine.h"

void scan_next_token(Parser* p) {
    const char* s = p->cursor;
    p->text[0] = '\0';
    for (;;) {
        while (is_sql_whitespace(*s)) s++;
        if (s[0] == '-' && s[1] == '-') { while (*s && *s != '\n') s++; continue; }
        if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) s++;
            if (!*s) { p->failed = true; p->kind = TK_BAD; p->cursor = s; return; }
            s += 2; continue;
        }
        break;
    }
    size_t length = 0;
    if (!*s) p->kind = TK_END;
    else if (is_identifier_start(*s)) {
        p->kind = TK_ID;
        do {
            if (length + 1 >= sizeof(p->text)) { p->failed = true; break; }
            p->text[length++] = uppercase_ascii(*s++);
        } while (is_identifier_start(*s) || is_ascii_digit(*s) || *s == '$' || *s == '#');
    } else if (is_ascii_digit(*s) || ((*s == '-' || *s == '+' || *s == '.') &&
               (is_ascii_digit(s[1]) || ((*s == '-' || *s == '+') && s[1] == '.' && is_ascii_digit(s[2]))))) {
        p->kind = TK_NUMBER;
        do {
            if (length + 1 >= sizeof(p->text)) { p->failed = true; break; }
            p->text[length++] = *s++;
        } while (is_ascii_digit(*s) || *s == '.');
    } else if (*s == '[') {
        s++; p->kind = TK_ID; bool closed = false;
        while (*s) {
            if (*s == ']') { s++; closed = true; break; }
            if (length + 1 >= sizeof(p->text)) { p->failed = true; break; }
            p->text[length++] = uppercase_ascii(*s++);
        }
        if (!closed || length == 0) p->failed = true;
    } else if (*s == '\'') {
        s++; p->kind = TK_STRING; bool closed = false;
        while (*s) {
            if (*s == '\'' && s[1] != '\'') { s++; closed = true; break; }
            if (length + 1 >= sizeof(p->text)) { p->failed = true; break; }
            p->text[length++] = *s;
            if (*s == '\'') s++;
            s++;
        }
        if (!closed) p->failed = true;
    } else {
        char c = *s++;
        switch (c) {
        case '(': p->kind = TK_LP; break; case ')': p->kind = TK_RP; break;
        case ',': p->kind = TK_COMMA; break; case '*': p->kind = TK_STAR; break;
        case '.': p->kind = TK_DOT; break; case ';': p->kind = TK_SEMI; break;
        case '+': p->kind = TK_PLUS; break; case '-': p->kind = TK_MINUS; break;
        case '/': p->kind = TK_SLASH; break;
        case '?': p->kind = TK_HELP; break;
        case '=': p->kind = TK_EQ; break;
        case '<':
            if (s[0] == '=' && s[1] == '>') { s += 2; p->kind = TK_COSINE_DIST; }
            else if (s[0] == '-' && s[1] == '>') { s += 2; p->kind = TK_L2_DIST; }
            else if (*s == '=') { s++; p->kind = TK_LE; }
            else if (*s == '>') { s++; p->kind = TK_NE; }
            else p->kind = TK_LT;
            break;
        case '>': p->kind = *s == '=' ? (s++, TK_GE) : TK_GT; break;
        case '!': if (*s == '=') { s++; p->kind = TK_NE; } else p->failed = true; break;
        default: p->failed = true; break;
        }
    }
    p->text[length] = '\0';
    p->cursor = s;
    if (p->failed) p->kind = TK_BAD;
}

bool token_is_keyword(Parser* p, const char* word) { return p->kind == TK_ID && !strcmp(p->text, word); }
bool consume_token(Parser* p, TokenKind kind) {
    if (p->kind != kind) return false;
    scan_next_token(p); return true;
}
bool consume_keyword(Parser* p, const char* word) {
    if (!token_is_keyword(p, word)) return false;
    scan_next_token(p); return true;
}
bool parse_identifier(Parser* p, char* output) {
    if (p->kind != TK_ID || !is_valid_identifier(p->text)) return false;
    memcpy(output, p->text, strlen(p->text) + 1);
    scan_next_token(p); return true;
}
bool consume_statement_end(Parser* p) {
    consume_token(p, TK_SEMI);
    return p->kind == TK_END && !p->failed;
}

bool parse_typed_literal(Parser* p, const ColumnDef* column, Value* value) {
    memset(value, 0, sizeof(*value));
    if (consume_keyword(p, "NULL")) { value->is_null = true; return true; }
    if (column->type == TYPE_NUMBER) {
        if (p->kind != TK_NUMBER || !normalize_decimal(p->text, value->text)) return false;
    } else {
        if (p->kind != TK_STRING) return false;
        memcpy(value->text, p->text, strlen(p->text) + 1);
        if (column->type == TYPE_DATE) {
            if (!is_valid_datetime(value->text)) return false;
            if (strlen(value->text) == 10) memcpy(value->text + 10, " 00:00:00", 10);
        }
    }
    scan_next_token(p);
    /* Nullability is checked on the candidate database, not on WHERE literals. */
    ColumnDef nullable = *column; nullable.nullable = true;
    return is_valid_column_value(&nullable, value);
}
