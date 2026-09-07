#ifndef MUJSCOMPILER_H
#define MUJSCOMPILER_H

#ifndef BUILD_WITH_MUJS
#error "mujscompiler.h: #define BUILD_WITH_MUJS before #include \"build.h\" -- \
then #include \"mujscompiler.h\" after it. Without BUILD_WITH_MUJS defined \
first, build.h already compiled no-op stubs of these functions, and this \
header can't safely replace them."
#endif

#include <mujs.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* =========================================================================
 * PART 1 -- syntax checking (unchanged in spirit: parse-only sanity net)
 * ========================================================================= */

static void mujs_syntax_check(const char *code, const char *label) {
    const char *what = label ? label : "<bundle>";

    js_State *J = js_newstate(NULL, NULL, JS_STRICT);
    if (!J) {
        fprintf(stderr, "mujscompiler: could not create MuJS state\n");
        exit(1);
    }

    if (js_ploadstring(J, what, code)) {
        fprintf(stderr, "mujscompiler: syntax error in %s:\n  %s\n",
                what, js_trystring(J, -1, "error"));
        js_freestate(J);
        exit(1);
    }

    js_pop(J, 1); /* discard the compiled function -- we only wanted to know it compiles */
    js_freestate(J);
}

static void mujs_check_all(const char *const *paths, size_t count, const char *strip_prefix) {
    for (size_t i = 0; i < count; i++) {
        long len;
        char *content = build__read_file(paths[i], &len);

        const char *display = paths[i];
        if (strip_prefix) {
            size_t plen = strlen(strip_prefix);
            if (strncmp(paths[i], strip_prefix, plen) == 0) display = paths[i] + plen;
        }

        mujs_syntax_check(content, display);
        free(content);
    }
    printf("mujscompiler: %zu file(s) OK\n", count);
}

static void mujs_check_bundle(const build_t *b) {
    char *tmp = malloc(b->out_len + 1);
    if (!tmp) { perror("malloc"); exit(1); }
    memcpy(tmp, b->out, b->out_len);
    tmp[b->out_len] = '\0';

    mujs_syntax_check(tmp, "bundle");
    free(tmp);

    printf("mujscompiler: bundle OK (%zu bytes)\n", b->out_len);
}

/* =========================================================================
 * PART 2 -- the site DSL compiler
 *
 * Site scripts (src/sites/<name>/script.js) are written in a small
 * sugar-on-top-of-JS DSL:
 *
 *   define KAGI {
 *       url("wildcard-scheme-and-host-pattern, e.g. star colon-slash-slash kagi.com slash star")
 *       loop("RESULT", ".search-result")
 *
 *       function ANIMATE(el) {
 *           el.classList.add("mu-pulse");
 *       }
 *
 *       focus(gi, ".search-input")
 *       click(gI, ANIMATE)
 *       focus(j, goto(next, "RESULT"))
 *       focus(k, goto(prev, "RESULT"))
 *   }
 *
 * (see the src/sites subfolders for real, working examples)
 *
 * The ONLY sugar is `define NAME{ ... }`, rewritten to
 * `define("NAME", function(){ ... });` before mujs ever sees it, plus
 * bare key/dir identifiers (`gi`, `next`, ...) as the first argument to
 * focus/click/longpress/doubleclick/goto, auto-quoted the same way.
 * `function NAME(args) { ... }` is plain, ordinary JavaScript.
 *
 * `focus`/`click`/`longpress`/`doubleclick` accept a selector string, a
 * function identifier (its ORIGINAL source is captured at the text level,
 * since mujs doesn't retain source for us, then tagged with a hidden
 * $muname property so the native callback can look it back up), or the
 * result of goto(dir, "LOOPNAME").
 *
 * Compiling every folder under src/sites/ dynamically discovers new
 * sites -- dropping in src/sites/newsite/script.js is enough, no build.c
 * edits required.
 * ========================================================================= */

#ifndef MUJS_MAX_BINDINGS
#define MUJS_MAX_BINDINGS 512
#endif
#ifndef MUJS_MAX_LOOPS
#define MUJS_MAX_LOOPS 64
#endif
#ifndef MUJS_MAX_MATCH
#define MUJS_MAX_MATCH 16
#endif
#ifndef MUJS_MAX_FUNCS
#define MUJS_MAX_FUNCS 128
#endif
#ifndef MUJS_VALUE_LEN
#define MUJS_VALUE_LEN 8192           /* selector / captured function source */
#endif
#ifndef MUJS_MAX_FRAMES
#define MUJS_MAX_FRAMES 128
#endif

typedef struct {
    char keys[64];             /* e.g. "gi", "j", "xx" -- any key sequence */
    char action[16];           /* focus | click | longpress | doubleclick */
    char kind[16];             /* selector | function | goto */
    char value[MUJS_VALUE_LEN]; /* selector text, or captured function source */
    char dir[8];                /* next | prev -- only used when kind == goto */
    char loopname[64];          /* only used when kind == goto */
} mujs_binding_t;

typedef struct {
    char name[64];
    char selector[512];
} mujs_loop_t;

typedef struct {
    char name[64];
    char match[MUJS_MAX_MATCH][256];
    int  match_count;
    mujs_loop_t loops[MUJS_MAX_LOOPS];
    int  loop_count;
    mujs_binding_t bindings[MUJS_MAX_BINDINGS];
    int  binding_count;
} mujs_site_t;

/* name -> exact original source of a `function NAME(...) {...}` decl,
 * captured while preprocessing (mujs itself keeps no source text). */
typedef struct {
    char name[64];
    char source[MUJS_VALUE_LEN];
} mujs_func_t;

typedef struct {
    js_State *J;
    mujs_site_t site;               /* accumulator for the `define` in flight */
    mujs_func_t funcs[MUJS_MAX_FUNCS]; /* function-source table, per compiled file */
    int func_count;
} mujs_dsl_t;

/* ---- preprocessing: define-sugar, function tagging, key auto-quoting -- */

enum { MUJS_FRAME_PLAIN, MUJS_FRAME_DEFINE, MUJS_FRAME_FUNCTION };

static const char *MUJS_KEYWORD_ARGS[] = { "focus", "click", "longpress", "doubleclick", "goto", "gotourl", NULL };

static char *mujs__preprocess(const char *src, mujs_dsl_t *dsl) {
    size_t srclen = strlen(src);
    char *out = malloc(srclen * 2 + 4096);
    if (!out) { perror("malloc"); exit(1); }
    size_t o = 0;

    int frame_kind[MUJS_MAX_FRAMES];
    size_t frame_start[MUJS_MAX_FRAMES];
    char frame_name[MUJS_MAX_FRAMES][64];
    int top = 0;

    dsl->func_count = 0;

    size_t i = 0;
    while (i < srclen) {
        char c = src[i];

        /* strings pass through verbatim */
        if (c == '"' || c == '\'') {
            char quote = c;
            out[o++] = src[i++];
            while (i < srclen && src[i] != quote) {
                if (src[i] == '\\' && i + 1 < srclen) out[o++] = src[i++];
                out[o++] = src[i++];
            }
            if (i < srclen) out[o++] = src[i++];
            continue;
        }
        /* comments pass through verbatim */
        if (c == '/' && i + 1 < srclen && src[i + 1] == '/') {
            while (i < srclen && src[i] != '\n') out[o++] = src[i++];
            continue;
        }
        if (c == '/' && i + 1 < srclen && src[i + 1] == '*') {
            out[o++] = src[i++]; out[o++] = src[i++];
            while (i + 1 < srclen && !(src[i] == '*' && src[i + 1] == '/')) out[o++] = src[i++];
            if (i + 1 < srclen) { out[o++] = src[i++]; out[o++] = src[i++]; }
            continue;
        }

        /* `define NAME{` sugar -> define("NAME", function(){ */
        if ((i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_')) &&
            strncmp(src + i, "define", 6) == 0 &&
            !isalnum((unsigned char)src[i + 6]) && src[i + 6] != '_') {
            size_t j = i + 6;
            while (j < srclen && isspace((unsigned char)src[j])) j++;
            size_t ns = j;
            while (j < srclen && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            size_t ne = j;
            size_t k = j;
            while (k < srclen && isspace((unsigned char)src[k])) k++;

            if (ne > ns && k < srclen && src[k] == '{') {
                o += snprintf(out + o, 256, "define(\"%.*s\", function(){", (int)(ne - ns), src + ns);
                if (top >= MUJS_MAX_FRAMES) { fprintf(stderr, "mujscompiler: nesting too deep\n"); exit(1); }
                frame_kind[top++] = MUJS_FRAME_DEFINE;
                i = k + 1;
                continue;
            }
        }

        /* `function NAME(` declaration -- capture source, tag it, don't rewrite it */
        if ((i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_')) &&
            strncmp(src + i, "function", 8) == 0 &&
            !isalnum((unsigned char)src[i + 8]) && src[i + 8] != '_') {
            size_t j = i + 8;
            while (j < srclen && isspace((unsigned char)src[j])) j++;
            size_t ns = j;
            while (j < srclen && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            size_t ne = j;
            size_t k = j;
            while (k < srclen && isspace((unsigned char)src[k])) k++;

            if (ne > ns && k < srclen && src[k] == '(') {
                size_t namelen = ne - ns;
                if (namelen >= sizeof(frame_name[0])) namelen = sizeof(frame_name[0]) - 1;
                size_t p = k;
                int pdepth = 1; p++;
                while (p < srclen && pdepth > 0) {
                    if (src[p] == '(') pdepth++;
                    else if (src[p] == ')') pdepth--;
                    p++;
                }
                while (p < srclen && isspace((unsigned char)src[p])) p++;
                if (p < srclen && src[p] == '{') {
                    size_t chunk = (p + 1) - i;
                    memcpy(out + o, src + i, chunk);
                    o += chunk;
                    if (top >= MUJS_MAX_FRAMES) { fprintf(stderr, "mujscompiler: nesting too deep\n"); exit(1); }
                    frame_kind[top] = MUJS_FRAME_FUNCTION;
                    frame_start[top] = i;
                    memcpy(frame_name[top], src + ns, namelen);
                    frame_name[top][namelen] = '\0';
                    top++;
                    i = p + 1;
                    continue;
                }
            }
        }

        /* auto-quote a bare key/dir identifier as the first arg to
         * focus/click/longpress/doubleclick/goto */
        {
            int matched_kw = -1;
            for (int kwi = 0; MUJS_KEYWORD_ARGS[kwi]; kwi++) {
                size_t klen = strlen(MUJS_KEYWORD_ARGS[kwi]);
                if ((i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_')) &&
                    strncmp(src + i, MUJS_KEYWORD_ARGS[kwi], klen) == 0 &&
                    !isalnum((unsigned char)src[i + klen]) && src[i + klen] != '_') {
                    matched_kw = kwi;
                    break;
                }
            }
            if (matched_kw >= 0) {
                size_t klen = strlen(MUJS_KEYWORD_ARGS[matched_kw]);
                size_t j = i + klen;
                while (j < srclen && isspace((unsigned char)src[j])) j++;
                if (j < srclen && src[j] == '(') {
                    size_t chunk = (j + 1) - i;
                    memcpy(out + o, src + i, chunk); o += chunk;
                    i = j + 1;
                    while (i < srclen && isspace((unsigned char)src[i])) out[o++] = src[i++];
                    if (i < srclen && (isalpha((unsigned char)src[i]) || src[i] == '_')) {
                        size_t ts = i;
                        while (i < srclen && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
                        size_t te = i;
                        size_t p2 = i;
                        while (p2 < srclen && isspace((unsigned char)src[p2])) p2++;
                        if (p2 < srclen && (src[p2] == ',' || src[p2] == ')')) {
                            out[o++] = '"';
                            memcpy(out + o, src + ts, te - ts); o += (te - ts);
                            out[o++] = '"';
                        } else {
                            memcpy(out + o, src + ts, te - ts); o += (te - ts);
                        }
                    }
                    continue;
                }
            }
        }

        if (c == '{') {
            if (top >= MUJS_MAX_FRAMES) { fprintf(stderr, "mujscompiler: nesting too deep\n"); exit(1); }
            frame_kind[top++] = MUJS_FRAME_PLAIN;
            out[o++] = src[i++];
            continue;
        }
        if (c == '}') {
            out[o++] = src[i++];
            if (top > 0) {
                top--;
                if (frame_kind[top] == MUJS_FRAME_DEFINE) {
                    out[o++] = ')'; out[o++] = ';';
                } else if (frame_kind[top] == MUJS_FRAME_FUNCTION) {
                    if (dsl->func_count < MUJS_MAX_FUNCS) {
                        mujs_func_t *f = &dsl->funcs[dsl->func_count++];
                        snprintf(f->name, sizeof(f->name), "%s", frame_name[top]);
                        size_t len = i - frame_start[top];
                        if (len >= sizeof(f->source)) len = sizeof(f->source) - 1;
                        memcpy(f->source, src + frame_start[top], len);
                        f->source[len] = '\0';
                        o += snprintf(out + o, 128, "\n%s.$muname = \"%s\";\n", f->name, f->name);
                    } else {
                        fprintf(stderr, "mujscompiler: too many function() decls (raise MUJS_MAX_FUNCS)\n");
                        exit(1);
                    }
                }
            }
            continue;
        }

        out[o++] = src[i++];
    }
    out[o] = '\0';
    return out;
}

/* ---- native callbacks exposed to site scripts --------------------------- */

static mujs_dsl_t *mujs__ctx(js_State *J) { return (mujs_dsl_t *)js_getcontext(J); }

static void mujs__reject(js_State *J, const char *what) {
    js_newerror(J, what);
    js_throw(J);
}

static void mujs_native_url(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->match_count >= MUJS_MAX_MATCH) mujs__reject(J, "too many url() patterns (raise MUJS_MAX_MATCH)");
    snprintf(s->match[s->match_count++], 256, "%s", js_tostring(J, 1));
    js_pushundefined(J);
}

static void mujs_native_loop(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->loop_count >= MUJS_MAX_LOOPS) mujs__reject(J, "too many loop() groups (raise MUJS_MAX_LOOPS)");
    mujs_loop_t *l = &s->loops[s->loop_count++];
    snprintf(l->name, sizeof(l->name), "%s", js_tostring(J, 1));
    snprintf(l->selector, sizeof(l->selector), "%s", js_tostring(J, 2));
    js_pushundefined(J);
}

/* goto(next|prev, "LOOPNAME") -- returns a marker object; focus()/click()/
 * etc interpret it, goto() itself performs no action. */
static void mujs_native_goto(js_State *J) {
    const char *dir = js_tostring(J, 1);
    const char *loopname = js_tostring(J, 2);
    js_newobject(J);
    js_pushliteral(J, "__mugoto__");
    js_setproperty(J, -2, "$type");
    js_pushstring(J, dir);
    js_setproperty(J, -2, "dir");
    js_pushstring(J, loopname);
    js_setproperty(J, -2, "loop");
}

/* gotourl(key, "url") -- a binding declarator like focus/click/etc, not a
 * target resolver: pressing `key` just navigates the page there. Accepts
 * absolute ("https://...") or relative ("/settings") URLs, used as-is by
 * core.js's `location.href = ...`. */
static void mujs_native_gotourl(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "navigate");
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->kind, sizeof(b->kind), "url");
    snprintf(b->value, sizeof(b->value), "%s", js_tostring(J, 2));
    js_pushundefined(J);
}

static const char *mujs__lookup_func_source(mujs_dsl_t *dsl, const char *name) {
    for (int i = 0; i < dsl->func_count; i++)
        if (strcmp(dsl->funcs[i].name, name) == 0)
            return dsl->funcs[i].source;
    return NULL;
}

static void mujs__bind(js_State *J, const char *action) {
    mujs_dsl_t *dsl = mujs__ctx(J);
    mujs_site_t *s = &dsl->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "%s", action);
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));

    if (js_isstring(J, 2)) {
        snprintf(b->kind, sizeof(b->kind), "selector");
        snprintf(b->value, sizeof(b->value), "%s", js_tostring(J, 2));

    } else if (js_iscallable(J, 2)) {
        js_getproperty(J, 2, "$muname");
        if (!js_isstring(J, -1)) {
            js_pop(J, 1);
            mujs__reject(J, "function passed to focus()/click()/etc must be a named "
                            "top-level `function NAME(...) {...}` declaration");
        }
        char fname[64];
        snprintf(fname, sizeof(fname), "%s", js_tostring(J, -1));
        js_pop(J, 1);

        const char *source = mujs__lookup_func_source(dsl, fname);
        if (!source) mujs__reject(J, "could not find captured source for that function");
        snprintf(b->kind, sizeof(b->kind), "function");
        snprintf(b->value, sizeof(b->value), "%s", source);

    } else if (js_isobject(J, 2)) {
        js_getproperty(J, 2, "$type");
        int is_goto = js_isstring(J, -1) && strcmp(js_tostring(J, -1), "__mugoto__") == 0;
        js_pop(J, 1);
        if (!is_goto) mujs__reject(J, "focus()/click()/etc expects a selector, a function, or goto(...)");
        snprintf(b->kind, sizeof(b->kind), "goto");
        js_getproperty(J, 2, "dir");
        snprintf(b->dir, sizeof(b->dir), "%s", js_tostring(J, -1));
        js_pop(J, 1);
        js_getproperty(J, 2, "loop");
        snprintf(b->loopname, sizeof(b->loopname), "%s", js_tostring(J, -1));
        js_pop(J, 1);
    } else {
        mujs__reject(J, "focus()/click()/etc expects a selector, a function, or goto(...)");
    }
    js_pushundefined(J);
}

static void mujs_native_focus(js_State *J)       { mujs__bind(J, "focus"); }
static void mujs_native_click(js_State *J)       { mujs__bind(J, "click"); }
static void mujs_native_longpress(js_State *J)   { mujs__bind(J, "longpress"); }
static void mujs_native_doubleclick(js_State *J) { mujs__bind(J, "doubleclick"); }

static void mujs_native_define(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", js_tostring(J, 1));

    js_copy(J, 2);         /* the function value */
    js_pushundefined(J);   /* `this` */
    js_call(J, 0);
    js_pop(J, 1);           /* discard its return value */

    js_pushundefined(J);
}

static void mujs__register_natives(js_State *J) {
    js_newcfunction(J, mujs_native_define, "define", 2);        js_setglobal(J, "define");
    js_newcfunction(J, mujs_native_url, "url", 1);               js_setglobal(J, "url");
    js_newcfunction(J, mujs_native_loop, "loop", 2);             js_setglobal(J, "loop");
    js_newcfunction(J, mujs_native_goto, "goto", 2);             js_setglobal(J, "goto");
    js_newcfunction(J, mujs_native_gotourl, "gotourl", 2);       js_setglobal(J, "gotourl");
    js_newcfunction(J, mujs_native_focus, "focus", 2);           js_setglobal(J, "focus");
    js_newcfunction(J, mujs_native_click, "click", 2);           js_setglobal(J, "click");
    js_newcfunction(J, mujs_native_longpress, "longpress", 2);   js_setglobal(J, "longpress");
    js_newcfunction(J, mujs_native_doubleclick, "doubleclick", 2); js_setglobal(J, "doubleclick");
}

/* ---- serialize a compiled mujs_site_t into Sites.register({...}) ------- */

static void mujs__json_escape(char *dst, size_t dst_cap, size_t *dst_len, const char *s) {
    dst[(*dst_len)++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p && *dst_len + 8 < dst_cap; p++) {
        switch (*p) {
        case '"':  dst[(*dst_len)++] = '\\'; dst[(*dst_len)++] = '"'; break;
        case '\\': dst[(*dst_len)++] = '\\'; dst[(*dst_len)++] = '\\'; break;
        case '\n': dst[(*dst_len)++] = '\\'; dst[(*dst_len)++] = 'n'; break;
        case '\r': dst[(*dst_len)++] = '\\'; dst[(*dst_len)++] = 'r'; break;
        case '\t': dst[(*dst_len)++] = '\\'; dst[(*dst_len)++] = 't'; break;
        default:
            if (*p < 0x20) *dst_len += snprintf(dst + *dst_len, 8, "\\u%04x", *p);
            else dst[(*dst_len)++] = (char)*p;
        }
    }
    dst[(*dst_len)++] = '"';
}

static char *mujs__emit_site(const mujs_site_t *s) {
    size_t cap = MUJS_VALUE_LEN * ((size_t)s->binding_count + 4) + 4096;
    char *out = malloc(cap);
    if (!out) { perror("malloc"); exit(1); }
    size_t n = 0;

    n += snprintf(out + n, cap - n, "Sites.register({\n  name: ");
    mujs__json_escape(out, cap, &n, s->name);
    n += snprintf(out + n, cap - n, ",\n  match: [");
    for (int i = 0; i < s->match_count; i++) {
        if (i) { out[n++] = ','; out[n++] = ' '; }
        mujs__json_escape(out, cap, &n, s->match[i]);
    }
    n += snprintf(out + n, cap - n, "],\n  loops: {");
    for (int i = 0; i < s->loop_count; i++) {
        if (i) { out[n++] = ','; out[n++] = ' '; }
        mujs__json_escape(out, cap, &n, s->loops[i].name);
        out[n++] = ':'; out[n++] = ' ';
        mujs__json_escape(out, cap, &n, s->loops[i].selector);
    }
    n += snprintf(out + n, cap - n, "},\n  bindings: [\n");
    for (int i = 0; i < s->binding_count; i++) {
        const mujs_binding_t *b = &s->bindings[i];
        n += snprintf(out + n, cap - n, "    { keys: ");
        mujs__json_escape(out, cap, &n, b->keys);
        n += snprintf(out + n, cap - n, ", action: ");
        mujs__json_escape(out, cap, &n, b->action);
        n += snprintf(out + n, cap - n, ", kind: ");
        mujs__json_escape(out, cap, &n, b->kind);
        if (strcmp(b->kind, "selector") == 0) {
            n += snprintf(out + n, cap - n, ", value: ");
            mujs__json_escape(out, cap, &n, b->value);
        } else if (strcmp(b->kind, "url") == 0) {
            n += snprintf(out + n, cap - n, ", value: ");
            mujs__json_escape(out, cap, &n, b->value);
        } else if (strcmp(b->kind, "function") == 0) {
            n += snprintf(out + n, cap - n, ", value: (%s)", b->value); /* a REAL function, not a string */
        } else if (strcmp(b->kind, "goto") == 0) {
            n += snprintf(out + n, cap - n, ", dir: ");
            mujs__json_escape(out, cap, &n, b->dir);
            n += snprintf(out + n, cap - n, ", loop: ");
            mujs__json_escape(out, cap, &n, b->loopname);
        }
        n += snprintf(out + n, cap - n, " }%s\n", (i + 1 < s->binding_count) ? "," : "");
    }
    n += snprintf(out + n, cap - n, "  ]\n});\n\n");
    return out;
}

/* ---- public entry points -------------------------------------------------
 *
 * mujs_compile_site(b, path)     -- compile one site script into b directly
 * mujs_compile_sites_dir(b, dir) -- dynamically discover every
 *                                    <dir>/<name>/script.js and compile
 *                                    them all, in directory order. Drop a
 *                                    new folder in -- no build.c edits.
 */

static void mujs_compile_site(build_t *b, const char *path) {
    long len;
    char *raw = build__read_file(path, &len);

    mujs_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    dsl.J = js_newstate(NULL, NULL, JS_STRICT);
    if (!dsl.J) { fprintf(stderr, "mujscompiler: js_newstate failed\n"); exit(1); }
    js_setcontext(dsl.J, &dsl);
    mujs__register_natives(dsl.J);

    char *js = mujs__preprocess(raw, &dsl);
    free(raw);

    if (js_ploadstring(dsl.J, path, js)) {
        fprintf(stderr, "mujscompiler: parse error in %s:\n  %s\n", path, js_trystring(dsl.J, -1, "error"));
        exit(1);
    }
    free(js);

    js_pushundefined(dsl.J); /* `this` for the top-level script function */
    if (js_pcall(dsl.J, 0)) {
        fprintf(stderr, "mujscompiler: error compiling %s:\n  %s\n", path, js_trystring(dsl.J, -1, "error"));
        exit(1);
    }
    js_pop(dsl.J, 1);

    char *generated = mujs__emit_site(&dsl.site);
    build_raw(b, generated);
    free(generated);

    js_freestate(dsl.J);
}

static void mujs_compile_sites_dir(build_t *b, const char *sites_dir) {
    DIR *d = opendir(sites_dir);
    if (!d) {
        fprintf(stderr, "mujscompiler: no %s directory (nothing to compile)\n", sites_dir);
        return;
    }

    build_raw(b, "// ---- generated: compiled site definitions ----\n");

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char subpath[1024];
        snprintf(subpath, sizeof(subpath), "%s/%s", sites_dir, e->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char script[1040]; /* subpath (<=1024) + "/script.js" (10) + NUL, with headroom */
        snprintf(script, sizeof(script), "%s/script.js", subpath);
        FILE *f = fopen(script, "rb");
        if (!f) {
            fprintf(stderr, "mujscompiler: skipping %s (no script.js)\n", subpath);
            continue;
        }
        fclose(f);

        printf("mujscompiler: compiling %s\n", script);
        mujs_compile_site(b, script);
    }
    closedir(d);
}

#endif /* MUJSCOMPILER_H */
