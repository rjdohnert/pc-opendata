#include "database_engine.h"

bool is_identifier_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool is_sql_whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
char uppercase_ascii(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

bool is_valid_identifier(const char* text) {
    if (!is_identifier_start(*text) || strlen(text) >= MAX_NAME_LEN) return false;
    for (; *text; text++)
        if (!(is_identifier_start(*text) || is_ascii_digit(*text) || *text == '$' || *text == '#') ||
            uppercase_ascii(*text) != *text) return false;
    return true;
}

/* Decimal normalization avoids binary floating-point rounding in keys and comparisons. */
bool normalize_decimal(const char* text, char* output) {
    bool negative = *text == '-';
    if (*text == '-' || *text == '+') text++;
    const char* integer = text;
    while (is_ascii_digit(*text)) text++;
    size_t integer_len = (size_t)(text - integer);
    const char* fraction = text;
    size_t fraction_len = 0;
    if (*text == '.') {
        fraction = ++text;
        while (is_ascii_digit(*text)) text++;
        fraction_len = (size_t)(text - fraction);
    }
    if (*text || integer_len + fraction_len == 0) return false;
    while (integer_len && *integer == '0') { integer++; integer_len--; }
    while (fraction_len && fraction[fraction_len - 1] == '0') fraction_len--;
    if (integer_len + fraction_len > 38 || fraction_len > 18) return false;
    char* out = output;
    if (negative && (integer_len || fraction_len)) *out++ = '-';
    if (integer_len) { memcpy(out, integer, integer_len); out += integer_len; }
    else *out++ = '0';
    if (fraction_len) {
        *out++ = '.';
        memcpy(out, fraction, fraction_len);
        out += fraction_len;
    }
    *out = '\0';
    return true;
}

static int compare_decimals(const char* a, const char* b) {
    bool an = *a == '-', bn = *b == '-';
    if (an != bn) return an ? -1 : 1;
    if (an) { a++; b++; }
    size_t ai = strcspn(a, "."), bi = strcspn(b, ".");
    int result = ai < bi ? -1 : ai > bi ? 1 : 0;
    if (!result) {
        int cmp = strncmp(a, b, ai);
        result = cmp < 0 ? -1 : cmp > 0 ? 1 : 0;
    }
    if (!result) {
        const char* af = a + ai; const char* bf = b + bi;
        if (*af) af++;
        if (*bf) bf++;
        while (*af || *bf) {
            char ac = *af ? *af++ : '0', bc = *bf ? *bf++ : '0';
            if (ac != bc) { result = ac < bc ? -1 : 1; break; }
        }
    }
    return an ? -result : result;
}

bool is_valid_datetime(const char* text) {
    size_t length = strlen(text);
    if (length != 10 && length != 19) return false;
    for (size_t i = 0; i < length; i++) {
        if (i == 4 || i == 7) { if (text[i] != '-') return false; }
        else if (i == 10) { if (text[i] != ' ') return false; }
        else if (i == 13 || i == 16) { if (text[i] != ':') return false; }
        else if (!is_ascii_digit(text[i])) return false;
    }
    int y = (text[0]-'0')*1000 + (text[1]-'0')*100 + (text[2]-'0')*10 + text[3]-'0';
    int m = (text[5]-'0')*10 + text[6]-'0', d = (text[8]-'0')*10 + text[9]-'0';
    static const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (!y || m < 1 || m > 12 || d < 1) return false;
    int limit = days[m] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (d > limit) return false;
    return length == 10 ||
        (((text[11]-'0')*10 + text[12]-'0') < 24 &&
         ((text[14]-'0')*10 + text[15]-'0') < 60 &&
         ((text[17]-'0')*10 + text[18]-'0') < 60);
}

int parse_vector_floats(const char* text, float* out_vals, int max_dim) {
    if (!text) return -1;
    const char* s = text;
    while (is_sql_whitespace(*s)) s++;
    if (*s != '[') return -1;
    s++;
    int count = 0;
    while (*s) {
        while (is_sql_whitespace(*s)) s++;
        if (*s == ']') {
            s++;
            while (is_sql_whitespace(*s)) s++;
            return (*s == '\0' && count > 0) ? count : -1;
        }
        if (count >= max_dim) return -1;
        char* endptr = NULL;
        float val = strtof(s, &endptr);
        if (endptr == s || !isfinite(val)) return -1;
        if (out_vals) out_vals[count] = val;
        count++;
        s = endptr;
        while (is_sql_whitespace(*s)) s++;
        if (*s == ',') {
            s++;
            while (is_sql_whitespace(*s)) s++;
            if (*s == ']') return -1;
        } else if (*s != ']') {
            return -1;
        }
    }
    return -1;
}

bool vector_quantize_sq8(const float* vec, int dim, VectorSQ8* out_sq8) {
    if (!vec || dim <= 0 || dim > 64 || !out_sq8) return false;
    float max_abs = 0.0f;
    for (int i = 0; i < dim; i++) {
        float a = fabsf(vec[i]);
        if (a > max_abs) max_abs = a;
    }
    float scale = (max_abs > 1e-7f) ? (max_abs / 127.0f) : 1.0f;
    out_sq8->scale = scale;
    out_sq8->dim = dim;
    for (int i = 0; i < dim; i++) {
        float q = roundf(vec[i] / scale);
        if (q > 127.0f) q = 127.0f;
        if (q < -128.0f) q = -128.0f;
        out_sq8->q[i] = (int8_t)q;
    }
    return true;
}

double vector_sq8_cosine_distance(const VectorSQ8* a, const VectorSQ8* b) {
    if (!a || !b || a->dim <= 0 || a->dim != b->dim) return 1.0;
    int32_t dot_q = 0;
    int32_t na_q = 0;
    int32_t nb_q = 0;
    int dim = a->dim;
    for (int i = 0; i < dim; i++) {
        int32_t qa = (int32_t)a->q[i];
        int32_t qb = (int32_t)b->q[i];
        dot_q += qa * qb;
        na_q += qa * qa;
        nb_q += qb * qb;
    }
    double denom = sqrt((double)na_q) * sqrt((double)nb_q);
    if (denom <= 1e-9) return 1.0;
    double sim = (double)dot_q / denom;
    if (sim > 1.0) sim = 1.0;
    if (sim < -1.0) sim = -1.0;
    return 1.0 - sim;
}

double vector_sq8_l2_distance(const VectorSQ8* a, const VectorSQ8* b) {
    if (!a || !b || a->dim <= 0 || a->dim != b->dim) return 1e30;
    double sum_sq = 0.0;
    double sa = (double)a->scale;
    double sb = (double)b->scale;
    int dim = a->dim;
    for (int i = 0; i < dim; i++) {
        double diff = ((double)a->q[i] * sa) - ((double)b->q[i] * sb);
        sum_sq += diff * diff;
    }
    return sqrt(sum_sq);
}

double vector_cosine_distance(const char* a, const char* b) {
    float va[64], vb[64];
    int da = parse_vector_floats(a, va, 64);
    int db = parse_vector_floats(b, vb, 64);
    if (da <= 0 || da != db) return 1.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    int i = 0;
    for (; i + 4 <= da; i += 4) {
        double a0 = (double)va[i],     b0 = (double)vb[i];
        double a1 = (double)va[i + 1], b1 = (double)vb[i + 1];
        double a2 = (double)va[i + 2], b2 = (double)vb[i + 2];
        double a3 = (double)va[i + 3], b3 = (double)vb[i + 3];
        dot += (a0 * b0 + a1 * b1) + (a2 * b2 + a3 * b3);
        na  += (a0 * a0 + a1 * a1) + (a2 * a2 + a3 * a3);
        nb  += (b0 * b0 + b1 * b1) + (b2 * b2 + b3 * b3);
    }
    for (; i < da; i++) {
        double ai = (double)va[i], bi = (double)vb[i];
        dot += ai * bi;
        na  += ai * ai;
        nb  += bi * bi;
    }
    double denom = sqrt(na) * sqrt(nb);
    if (denom <= 1e-12) return 1.0;
    double sim = dot / denom;
    if (sim > 1.0) sim = 1.0;
    if (sim < -1.0) sim = -1.0;
    return 1.0 - sim;
}

double vector_l2_distance(const char* a, const char* b) {
    float va[64], vb[64];
    int da = parse_vector_floats(a, va, 64);
    int db = parse_vector_floats(b, vb, 64);
    if (da <= 0 || da != db) return 1e30;
    double sum_sq = 0.0;
    int i = 0;
    for (; i + 4 <= da; i += 4) {
        double d0 = (double)va[i]     - (double)vb[i];
        double d1 = (double)va[i + 1] - (double)vb[i + 1];
        double d2 = (double)va[i + 2] - (double)vb[i + 2];
        double d3 = (double)va[i + 3] - (double)vb[i + 3];
        sum_sq += (d0 * d0 + d1 * d1) + (d2 * d2 + d3 * d3);
    }
    for (; i < da; i++) {
        double di = (double)va[i] - (double)vb[i];
        sum_sq += di * di;
    }
    return sqrt(sum_sq);
}

int compare_typed_values(DataType type, const Value* a, const Value* b) {
    if (type == TYPE_NUMBER) return compare_decimals(a->text, b->text);
    return strcmp(a->text, b->text);
}

bool is_valid_column_value(const ColumnDef* column, const Value* value) {
    if (value->is_null) return column->nullable;
    if (column->type == TYPE_VECTOR) {
        if (strlen(value->text) >= MAX_VAL_LEN) return false;
        float vec[64];
        int dim = parse_vector_floats(value->text, vec, 64);
        return dim == column->size;
    }
    if (strlen(value->text) > (size_t)column->size) return false;
    if (column->type == TYPE_NUMBER) {
        char normalized[MAX_VAL_LEN];
        return normalize_decimal(value->text, normalized) && strcmp(normalized, value->text) == 0;
    }
    return column->type != TYPE_DATE || (strlen(value->text) == 19 && is_valid_datetime(value->text));
}
