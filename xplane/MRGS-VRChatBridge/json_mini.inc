
/* ---------------- minimal JSON DOM ----------------
 * Just enough for the plugin's own state file: objects, arrays,
 * strings (full escapes incl. \uXXXX -> UTF-8; Python's json.dump
 * escapes all non-ASCII, so raw bytes >= 0x80 never appear),
 * numbers, bools, null. Depth-capped; any parse error -> NULL. */

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } JT;
typedef struct JVal JVal;
struct JVal {
    JT t;
    double num;
    char *str;
    JVal **it;
    char **keys;
    int n, cap;
};

static JVal *jnew(JT t)
{
    JVal *v = (JVal *)calloc(1, sizeof(JVal));
    if (v) v->t = t;
    return v;
}

static void jfree(JVal *v)
{
    if (!v) return;
    free(v->str);
    for (int i = 0; i < v->n; i++) {
        jfree(v->it[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->it);
    free(v->keys);
    free(v);
}

static int jpush(JVal *arr, JVal *item, char *key)
{
    if (arr->n >= arr->cap) {
        int nc = arr->cap ? arr->cap * 2 : 8;
        JVal **ni = (JVal **)realloc(arr->it, nc * sizeof(JVal *));
        if (!ni) return 0;
        arr->it = ni;
        if (arr->t == JOBJ) {
            char **nk = (char **)realloc(arr->keys, nc * sizeof(char *));
            if (!nk) return 0;
            arr->keys = nk;
        }
        arr->cap = nc;
    }
    arr->it[arr->n] = item;
    if (arr->t == JOBJ) arr->keys[arr->n] = key;
    arr->n++;
    return 1;
}

typedef struct { const char *p; int err; } JParse;

static void jws(JParse *ps)
{
    while (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\r' || *ps->p == '\t')
        ps->p++;
}

static int jhex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static char *jparse_str(JParse *ps)
{
    /* *ps->p == '"' on entry */
    ps->p++;
    const char *s = ps->p;
    size_t cap = 64, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) { ps->err = 1; return NULL; }
    while (*s && *s != '"') {
        unsigned int cp = 0;
        if (*s == '\\') {
            s++;
            switch (*s) {
                case 'n': cp = '\n'; s++; break;
                case 't': cp = '\t'; s++; break;
                case 'r': cp = '\r'; s++; break;
                case 'b': cp = '\b'; s++; break;
                case 'f': cp = '\f'; s++; break;
                case '/': cp = '/';  s++; break;
                case '\\': cp = '\\'; s++; break;
                case '"': cp = '"';  s++; break;
                case 'u': {
                    int h = jhex4(s + 1);
                    if (h < 0) { free(out); ps->err = 1; return NULL; }
                    s += 5;
                    cp = (unsigned int)h;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        s[0] == '\\' && s[1] == 'u') {
                        int lo = jhex4(s + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10)
                                 + (unsigned int)(lo - 0xDC00);
                            s += 6;
                        } else cp = '?';
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = '?';
                    break;
                }
                default: free(out); ps->err = 1; return NULL;
            }
        } else {
            cp = (unsigned char)*s;
            s++;
        }
        char enc[4];
        int en;
        if (cp < 0x80) { enc[0] = (char)cp; en = 1; }
        else if (cp < 0x800) {
            enc[0] = (char)(0xC0 | (cp >> 6));
            enc[1] = (char)(0x80 | (cp & 63)); en = 2;
        } else if (cp < 0x10000) {
            enc[0] = (char)(0xE0 | (cp >> 12));
            enc[1] = (char)(0x80 | ((cp >> 6) & 63));
            enc[2] = (char)(0x80 | (cp & 63)); en = 3;
        } else {
            enc[0] = (char)(0xF0 | (cp >> 18));
            enc[1] = (char)(0x80 | ((cp >> 12) & 63));
            enc[2] = (char)(0x80 | ((cp >> 6) & 63));
            enc[3] = (char)(0x80 | (cp & 63)); en = 4;
        }
        if (len + (size_t)en + 1 > cap) {
            cap = (cap + (size_t)en) * 2;
            char *no = (char *)realloc(out, cap);
            if (!no) { free(out); ps->err = 1; return NULL; }
            out = no;
        }
        memcpy(out + len, enc, (size_t)en);
        len += (size_t)en;
    }
    if (*s != '"') { free(out); ps->err = 1; return NULL; }
    ps->p = s + 1;
    out[len] = 0;
    return out;
}

static JVal *jparse_val(JParse *ps, int depth)
{
    if (depth > 48) { ps->err = 1; return NULL; }
    jws(ps);
    char c = *ps->p;
    if (c == '{') {
        ps->p++;
        JVal *o = jnew(JOBJ);
        if (!o) { ps->err = 1; return NULL; }
        jws(ps);
        if (*ps->p == '}') { ps->p++; return o; }
        for (;;) {
            jws(ps);
            if (*ps->p != '"') { ps->err = 1; jfree(o); return NULL; }
            char *key = jparse_str(ps);
            if (ps->err) { jfree(o); return NULL; }
            jws(ps);
            if (*ps->p != ':') { free(key); ps->err = 1; jfree(o); return NULL; }
            ps->p++;
            JVal *val = jparse_val(ps, depth + 1);
            if (ps->err) { free(key); jfree(o); return NULL; }
            if (!jpush(o, val, key)) { free(key); jfree(val); jfree(o); ps->err = 1; return NULL; }
            jws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == '}') { ps->p++; return o; }
            ps->err = 1; jfree(o); return NULL;
        }
    }
    if (c == '[') {
        ps->p++;
        JVal *a = jnew(JARR);
        if (!a) { ps->err = 1; return NULL; }
        jws(ps);
        if (*ps->p == ']') { ps->p++; return a; }
        for (;;) {
            JVal *val = jparse_val(ps, depth + 1);
            if (ps->err) { jfree(a); return NULL; }
            if (!jpush(a, val, NULL)) { jfree(val); jfree(a); ps->err = 1; return NULL; }
            jws(ps);
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == ']') { ps->p++; return a; }
            ps->err = 1; jfree(a); return NULL;
        }
    }
    if (c == '"') {
        char *s = jparse_str(ps);
        if (ps->err) return NULL;
        JVal *v = jnew(JSTR);
        if (!v) { free(s); ps->err = 1; return NULL; }
        v->str = s;
        return v;
    }
    if (c == 't' && strncmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        JVal *v = jnew(JBOOL);
        if (v) v->num = 1;
        return v;
    }
    if (c == 'f' && strncmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        return jnew(JBOOL);
    }
    if (c == 'n' && strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return jnew(JNULL);
    }
    char *end = NULL;
    double d = strtod(ps->p, &end);
    if (end && end != ps->p) {
        ps->p = end;
        JVal *v = jnew(JNUM);
        if (v) v->num = d;
        return v;
    }
    ps->err = 1;
    return NULL;
}

static JVal *json_parse(const char *text)
{
    JParse ps = { text, 0 };
    JVal *v = jparse_val(&ps, 0);
    if (ps.err) { jfree(v); return NULL; }
    return v;
}

/* accessors - NULL-safe, type-tolerant */
static const JVal *jg(const JVal *o, const char *key)
{
    if (!o || o->t != JOBJ) return NULL;
    for (int i = 0; i < o->n; i++)
        if (o->keys[i] && strcmp(o->keys[i], key) == 0)
            return o->it[i]->t == JNULL ? NULL : o->it[i];
    return NULL;
}

static const char *jgs(const JVal *o, const char *key, const char *def)
{
    const JVal *v = jg(o, key);
    return (v && v->t == JSTR && v->str) ? v->str : def;
}

static double jgn(const JVal *o, const char *key, double def, int *has)
{
    const JVal *v = jg(o, key);
    if (v && v->t == JNUM) { if (has) *has = 1; return v->num; }
    if (has) *has = 0;
    return def;
}

static int jgb(const JVal *o, const char *key, int def)
{
    const JVal *v = jg(o, key);
    if (v && v->t == JBOOL) return v->num ? 1 : 0;
    if (v && v->t == JNUM) return v->num != 0;
    return def;
}

static int jlen(const JVal *a)
{
    return (a && (a->t == JARR || a->t == JOBJ)) ? a->n : 0;
}

static const JVal *jat(const JVal *a, int i)
{
    if (!a || (a->t != JARR && a->t != JOBJ) || i < 0 || i >= a->n)
        return NULL;
    return a->it[i]->t == JNULL ? NULL : a->it[i];
}
