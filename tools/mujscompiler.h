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

#ifndef MUJS_MAX_SEL_LOOPS
#define MUJS_MAX_SEL_LOOPS 16
#endif

typedef struct {
    char keys[64];             /* e.g. "gi", "j", "xx" -- any key sequence */
    char action[16];           /* focus | click | longpress | doubleclick */
    char kind[16];             /* selector | function | goto */
    char value[MUJS_VALUE_LEN]; /* selector text, or captured function source */
    char dir[8];                /* next | prev -- only used when kind == goto */
    char loopname[64];          /* only used when kind == goto */
    char sel_loops[MUJS_MAX_SEL_LOOPS][64]; /* only used when kind == selected; empty = every loop */
    int  sel_loop_count;
    int  has_target;            /* only used when kind == yankurl */
    char target_kind[16];       /* only used when kind == yankurl: selector|function|goto|selected */
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

/* ---- user-defined aliases (src/alias.js) --------------------------------
 *
 * A plain text config, NOT run through mujs -- just lines like:
 *
 *   alias branch = br
 *   alias branch = prebr
 *   alias yankurl = yu
 *   alias off = 0
 *
 * meaning "REALNAME, also callable as ALIASNAME". Numeric alias names
 * (like `0`) are supported -- `0(x)` isn't valid JS syntax at all, so
 * this is a raw text substitution pass applied to every site script
 * BEFORE mujs__preprocess ever sees it, same spirit as the define{}
 * sugar. Optional: if src/alias.js doesn't exist, this is just a no-op.
 */
#ifndef MUJS_MAX_ALIASES
#define MUJS_MAX_ALIASES 64
#endif

typedef struct {
    char real[64];
    char alias[64];
} mujs_alias_entry_t;

static mujs_alias_entry_t mujs_g_aliases[MUJS_MAX_ALIASES];
static int mujs_g_alias_count = 0;

static void mujs_load_aliases(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return; /* optional file -- no aliases configured is fine */

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "alias", 5) != 0 || isalnum((unsigned char)p[5]) || p[5] == '_') continue;
        p += 5;
        while (isspace((unsigned char)*p)) p++;

        char *real_start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '=') p++;
        size_t real_len = (size_t)(p - real_start);

        while (isspace((unsigned char)*p)) p++;
        if (*p != '=') continue;
        p++;
        while (isspace((unsigned char)*p)) p++;

        char *alias_start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '\n' && *p != '\r') p++;
        size_t alias_len = (size_t)(p - alias_start);

        if (real_len == 0 || alias_len == 0 || real_len >= 64 || alias_len >= 64) continue;
        if (mujs_g_alias_count >= MUJS_MAX_ALIASES) {
            fprintf(stderr, "mujscompiler: too many aliases in %s (raise MUJS_MAX_ALIASES)\n", path);
            break;
        }

        mujs_alias_entry_t *e = &mujs_g_aliases[mujs_g_alias_count++];
        memcpy(e->real, real_start, real_len); e->real[real_len] = '\0';
        memcpy(e->alias, alias_start, alias_len); e->alias[alias_len] = '\0';
    }
    fclose(f);

    if (mujs_g_alias_count > 0) {
        printf("mujscompiler: loaded %d alias(es) from %s\n", mujs_g_alias_count, path);
    }
}

/* Rewrites every `ALIASNAME(` call in `src` to `REALNAME(`. Word-
 * boundary checked on both sides using isalnum()/underscore, which
 * naturally also does the right thing for numeric aliases -- `0` won't
 * match inside `10(` because '1' right before it is alnum too. Only
 * touches things immediately followed (ignoring whitespace) by '(', so
 * a real numeric literal used as a number elsewhere is never touched.
 * Strings/comments pass through untouched. */
static char *mujs__apply_aliases(const char *src) {
    size_t srclen = strlen(src);
    if (mujs_g_alias_count == 0) {
        char *copy = malloc(srclen + 1);
        if (!copy) { perror("malloc"); exit(1); }
        memcpy(copy, src, srclen + 1);
        return copy;
    }

    char *out = malloc(srclen + (size_t)mujs_g_alias_count * 64 + 4096);
    if (!out) { perror("malloc"); exit(1); }
    size_t o = 0;
    size_t i = 0;

    while (i < srclen) {
        char c = src[i];

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

        int matched = -1;
        for (int a = 0; a < mujs_g_alias_count; a++) {
            size_t alen = strlen(mujs_g_aliases[a].alias);
            int before_ok = (i == 0) || !(isalnum((unsigned char)src[i - 1]) || src[i - 1] == '_');
            if (before_ok && strncmp(src + i, mujs_g_aliases[a].alias, alen) == 0) {
                size_t after = i + alen;
                int after_ok = !(isalnum((unsigned char)src[after]) || src[after] == '_');
                if (after_ok) {
                    size_t k = after;
                    while (k < srclen && isspace((unsigned char)src[k])) k++;
                    if (k < srclen && src[k] == '(') { matched = a; break; }
                }
            }
        }

        if (matched >= 0) {
            size_t rlen = strlen(mujs_g_aliases[matched].real);
            memcpy(out + o, mujs_g_aliases[matched].real, rlen);
            o += rlen;
            i += strlen(mujs_g_aliases[matched].alias);
            continue;
        }

        out[o++] = src[i++];
    }
    out[o] = '\0';
    return out;
}

/* ---- variable()/ignore()/exclude()/include() ----------------------------
 *
 * Pure compile-time string helpers -- like loop(), but NOT usable with
 * goto() (there's deliberately no C-side registry for these; they never
 * reach Sites.register(...) at all). Simple enough to just be real JS,
 * executed once per compiled file before the site script itself runs.
 */
static const char *MUJS_PRELUDE =
    "var __muVars = {};\n"
    "function variable(name, selector) { __muVars[name] = selector; return selector; }\n"
    "function ignore(name, extra) { return __muVars[name] + ':not(' + extra + ')'; }\n"
    "function exclude(name, extra) { return __muVars[name] + ':not(:has(' + extra + '))'; }\n"
    "function include(name, extra) { return __muVars[name] + ':has(' + extra + ')'; }\n";

enum { MUJS_FRAME_PLAIN, MUJS_FRAME_DEFINE, MUJS_FRAME_FUNCTION };

typedef struct { const char *name; int quote_args; } mujs_kwspec_t;

/* quote_args = how many LEADING arguments are always simple bare words
 * (keys/directions) that should be auto-quoted. Anything past that is
 * left completely alone by this pass, since it may be an arbitrary
 * expression (a selector string, a function identifier, a nested
 * goto(...) call) that a naive comma-scan would mis-parse. */
static const mujs_kwspec_t MUJS_KEYWORD_ARGS[] = {
    { "focus", 1 }, { "click", 1 }, { "longpress", 1 }, { "doubleclick", 1 }, { "opennew", 1 },
    { "goto", 1 }, { "gotourl", 1 },
    { "scroll", 3 }, /* key, up/down, and percentage-or-"full" -- all three are simple bare words when given as identifiers */
    { "navigate", 2 }, /* key, prev/next */
    { "action", 2 }, /* key, close/reload */
    { "root", 1 }, /* key only */
    { "branch", 1 }, /* key only */
    { "off", 1 }, /* off(key) bare-key form; off(click(key)) is unaffected -- "click" isn't
                   * followed by ',' or ')' there, so it's correctly left unquoted */
    { NULL, 0 }
};

/* If *pi points at a bare identifier immediately followed (after
 * whitespace) by ',' or ')', wrap it in quotes in `out` and advance *pi
 * past it. Otherwise leaves *pi untouched -- the caller's normal
 * character-by-character loop handles whatever's actually there. */
/* If *pi points at a bare identifier immediately followed (after
 * whitespace) by ',' or ')', wraps it in quotes and advances *pi past
 * it. Otherwise leaves *pi and *po COMPLETELY untouched -- e.g. the
 * `action` in `off(action(r))` is a nested call, not a bare word, and
 * must be left for the main scanner loop to rediscover as its OWN
 * keyword occurrence (so `r` still gets auto-quoted). Consuming it
 * here even just to copy it through verbatim would skip past it and
 * prevent that rediscovery -- this was a real regression once. */
static void mujs__maybe_quote_bare_arg(const char *src, size_t srclen, size_t *pi, char *out, size_t *po) {
    size_t i = *pi;
    if (i < srclen && (isalpha((unsigned char)src[i]) || src[i] == '_')) {
        size_t ts = i;
        size_t scan = i;
        while (scan < srclen && (isalnum((unsigned char)src[scan]) || src[scan] == '_')) scan++;
        size_t te = scan;
        size_t p2 = scan;
        while (p2 < srclen && isspace((unsigned char)src[p2])) p2++;
        if (p2 < srclen && (src[p2] == ',' || src[p2] == ')')) {
            size_t o = *po;
            out[o++] = '"';
            memcpy(out + o, src + ts, te - ts); o += (te - ts);
            out[o++] = '"';
            *po = o;
            *pi = te;
        }
        /* else: nested call, not a bare word -- leave *pi and *po alone */
    }
}

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

        /* auto-quote bare key/dir identifiers as the leading argument(s)
         * to focus/click/longpress/doubleclick/goto/gotourl/scroll */
        {
            int matched_kw = -1;
            for (int kwi = 0; MUJS_KEYWORD_ARGS[kwi].name; kwi++) {
                size_t klen = strlen(MUJS_KEYWORD_ARGS[kwi].name);
                if ((i == 0 || (!isalnum((unsigned char)src[i - 1]) && src[i - 1] != '_')) &&
                    strncmp(src + i, MUJS_KEYWORD_ARGS[kwi].name, klen) == 0 &&
                    !isalnum((unsigned char)src[i + klen]) && src[i + klen] != '_') {
                    matched_kw = kwi;
                    break;
                }
            }
            if (matched_kw >= 0) {
                size_t klen = strlen(MUJS_KEYWORD_ARGS[matched_kw].name);
                size_t j = i + klen;
                while (j < srclen && isspace((unsigned char)src[j])) j++;
                if (j < srclen && src[j] == '(') {
                    size_t chunk = (j + 1) - i;
                    memcpy(out + o, src + i, chunk); o += chunk;
                    i = j + 1;

                    int n_args = MUJS_KEYWORD_ARGS[matched_kw].quote_args;
                    for (int argn = 0; argn < n_args; argn++) {
                        while (i < srclen && isspace((unsigned char)src[i])) out[o++] = src[i++];
                        size_t before = i;
                        mujs__maybe_quote_bare_arg(src, srclen, &i, out, &o);
                        if (i == before) break; /* not a bare identifier -- stop, leave the rest alone */
                        if (argn + 1 < n_args) {
                            while (i < srclen && isspace((unsigned char)src[i])) out[o++] = src[i++];
                            if (i < srclen && src[i] == ',') { out[o++] = src[i++]; }
                            else break; /* no further argument -- nothing more to quote */
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

/* Returns 1 (and leaves an unbind marker object on the stack) if this
 * call's second argument was omitted -- mujs pads missing args with
 * `undefined` up to a function's declared arity rather than reducing
 * the actual argument count, so checking js_isundefined(J, 2) is the
 * real signal, not js_gettop(). Used by every binding-declaring native
 * (focus/click/.../action/navigate/scroll/gotourl) so `off(fn(key))`
 * works uniformly across all of them. Returns 0 (stack untouched) if a
 * real second argument was given -- the caller proceeds normally. */
static int mujs__maybe_unbind_marker(js_State *J) {
    if (!js_isundefined(J, 2)) return 0;
    const char *keys = js_tostring(J, 1);
    js_newobject(J);
    js_pushliteral(J, "__muunbind__");
    js_setproperty(J, -2, "$type");
    js_pushstring(J, keys);
    js_setproperty(J, -2, "keys");
    return 1;
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

/* selected(...) -- a marker object like goto(...), but variadic:
 * selected() means every loop's current cursor item; selected("MSG",
 * "RESULT") restricts it to just those named loops. Performs no action
 * itself -- focus()/click()/etc apply the action to each resolved item. */
static void mujs_native_selected(js_State *J) {
    int argc = js_gettop(J) - 1; /* stack slot 0 is `this` */
    js_newobject(J);
    js_pushliteral(J, "__muselected__");
    js_setproperty(J, -2, "$type");
    js_newarray(J);
    for (int i = 0; i < argc; i++) {
        js_copy(J, i + 1);
        js_setindex(J, -2, i);
    }
    js_setproperty(J, -2, "loops");
}

/* gotourl(key, "url") -- a binding declarator like focus/click/etc, not a
 * target resolver: pressing `key` just navigates the page there. Accepts
 * absolute ("https://...") or relative ("/settings") URLs, used as-is by
 * core.js's `location.href = ...`. */
static void mujs_native_gotourl(js_State *J) {
    if (mujs__maybe_unbind_marker(J)) return;
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "navigate");
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->kind, sizeof(b->kind), "url");
    snprintf(b->value, sizeof(b->value), "%s", js_tostring(J, 2));
    js_pushundefined(J);
}

/* scroll(key, up/down, percentage) -- percentage is relative to the
 * viewport height (50 = half a screen, 100 = a full screen), same idea
 * as vimium's d/u vs space/shift-space. Pass the word `full` instead of
 * a number to scroll all the way to the top/bottom of the whole page,
 * not just one screen's worth. */
static void mujs_native_scroll(js_State *J) {
    if (mujs__maybe_unbind_marker(J)) return;
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");

    const char *dir = js_tostring(J, 2);
    if (strcmp(dir, "up") != 0 && strcmp(dir, "down") != 0)
        mujs__reject(J, "scroll() direction must be up or down");

    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "scroll");
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->kind, sizeof(b->kind), "scroll");
    snprintf(b->dir, sizeof(b->dir), "%s", dir);

    if (js_isstring(J, 3)) {
        if (strcmp(js_tostring(J, 3), "full") != 0)
            mujs__reject(J, "scroll()'s third argument must be a percentage number, or the word full");
        /* Infinity is a real JS global -- window.scrollBy clamps to the
         * document's actual top/bottom automatically, no core.js
         * special-casing needed. */
        snprintf(b->value, sizeof(b->value), "Infinity");
    } else {
        snprintf(b->value, sizeof(b->value), "%g", js_tonumber(J, 3));
    }
    js_pushundefined(J);
}

static const char *mujs__lookup_func_source(mujs_dsl_t *dsl, const char *name) {
    for (int i = 0; i < dsl->func_count; i++)
        if (strcmp(dsl->funcs[i].name, name) == 0)
            return dsl->funcs[i].source;
    return NULL;
}

/* Resolves a target VALUE sitting at stack index `idx` -- a selector
 * string, a captured function, a goto(...) marker, or a selected(...)
 * marker -- into b's kind/value/dir/loopname/sel_loops fields. Shared
 * by mujs__bind (focus/click/etc's 2nd argument) and yankurl()'s
 * optional argument, so both accept exactly the same target types. */
static void mujs__resolve_target(js_State *J, int idx, mujs_dsl_t *dsl, mujs_binding_t *b) {
    if (js_isstring(J, idx)) {
        snprintf(b->kind, sizeof(b->kind), "selector");
        snprintf(b->value, sizeof(b->value), "%s", js_tostring(J, idx));

    } else if (js_iscallable(J, idx)) {
        js_getproperty(J, idx, "$muname");
        if (!js_isstring(J, -1)) {
            js_pop(J, 1);
            mujs__reject(J, "function passed here must be a named "
                            "top-level `function NAME(...) {...}` declaration");
        }
        char fname[64];
        snprintf(fname, sizeof(fname), "%s", js_tostring(J, -1));
        js_pop(J, 1);

        const char *source = mujs__lookup_func_source(dsl, fname);
        if (!source) mujs__reject(J, "could not find captured source for that function");
        snprintf(b->kind, sizeof(b->kind), "function");
        snprintf(b->value, sizeof(b->value), "%s", source);

    } else if (js_isobject(J, idx)) {
        js_getproperty(J, idx, "$type");
        const char *type = js_isstring(J, -1) ? js_tostring(J, -1) : "";
        int is_goto = strcmp(type, "__mugoto__") == 0;
        int is_selected = strcmp(type, "__muselected__") == 0;
        js_pop(J, 1);

        if (is_selected) {
            snprintf(b->kind, sizeof(b->kind), "selected");
            js_getproperty(J, idx, "loops");
            int n = js_getlength(J, -1);
            if (n > MUJS_MAX_SEL_LOOPS) mujs__reject(J, "too many loop names passed to selected() (raise MUJS_MAX_SEL_LOOPS)");
            for (int i = 0; i < n; i++) {
                js_getindex(J, -1, i);
                snprintf(b->sel_loops[i], sizeof(b->sel_loops[i]), "%s", js_tostring(J, -1));
                js_pop(J, 1);
            }
            b->sel_loop_count = n;
            js_pop(J, 1); /* the loops array */
        } else if (is_goto) {
            snprintf(b->kind, sizeof(b->kind), "goto");
            js_getproperty(J, idx, "dir");
            snprintf(b->dir, sizeof(b->dir), "%s", js_tostring(J, -1));
            js_pop(J, 1);
            js_getproperty(J, idx, "loop");
            snprintf(b->loopname, sizeof(b->loopname), "%s", js_tostring(J, -1));
            js_pop(J, 1);
        } else {
            mujs__reject(J, "expected a selector, a function, goto(...), or selected(...)");
        }
    } else {
        mujs__reject(J, "expected a selector, a function, goto(...), or selected(...)");
    }
}

static void mujs__bind(js_State *J, const char *action) {
    if (mujs__maybe_unbind_marker(J)) return;

    mujs_dsl_t *dsl = mujs__ctx(J);
    mujs_site_t *s = &dsl->site;

    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "%s", action);
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));

    mujs__resolve_target(J, 2, dsl, b);
    js_pushundefined(J);
}

/* navigate(key, prev/next) -- steps through browser history, like the
 * back/forward buttons. */
static void mujs_native_navigate(js_State *J) {
    if (mujs__maybe_unbind_marker(J)) return;
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");

    const char *dir = js_tostring(J, 2);
    if (strcmp(dir, "prev") != 0 && strcmp(dir, "next") != 0)
        mujs__reject(J, "navigate() direction must be prev or next");

    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "navigate");
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->kind, sizeof(b->kind), "navigate");
    snprintf(b->dir, sizeof(b->dir), "%s", dir);
    js_pushundefined(J);
}

/* action(key, close/reload) -- browser-level tab actions, distinct from
 * navigate()'s history stepping. close is privileged via the
 * "window.close" grant in build.c. */
/* yankurl() / yankurl(target) -- a VALUE, like goto()/selected(), used
 * as action()'s second argument. Copies the current page's URL if
 * called bare, or resolves `target` (same target types as focus/click:
 * selector, function, goto(...), selected(...)) and copies THAT
 * element's link href instead. Performs no action itself -- action()
 * interprets the marker. */
static void mujs_native_yankurl(js_State *J) {
    mujs_dsl_t *dsl = mujs__ctx(J);
    mujs_binding_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    int has_target = !js_isundefined(J, 1);
    if (has_target) mujs__resolve_target(J, 1, dsl, &tmp);

    js_newobject(J);
    js_pushliteral(J, "__muyankurl__");
    js_setproperty(J, -2, "$type");
    js_pushboolean(J, has_target);
    js_setproperty(J, -2, "hasTarget");
    if (has_target) {
        js_pushstring(J, tmp.kind);     js_setproperty(J, -2, "targetKind");
        js_pushstring(J, tmp.value);    js_setproperty(J, -2, "targetValue");
        js_pushstring(J, tmp.dir);      js_setproperty(J, -2, "targetDir");
        js_pushstring(J, tmp.loopname); js_setproperty(J, -2, "targetLoop");
        js_newarray(J);
        for (int i = 0; i < tmp.sel_loop_count; i++) {
            js_pushstring(J, tmp.sel_loops[i]);
            js_setindex(J, -2, i);
        }
        js_setproperty(J, -2, "targetSelLoops");
    }
}

/* action(key, close/reload/yankurl(...)) -- browser-level tab actions.
 * close is privileged via the "window.close" grant, yankurl's copy is
 * privileged via the "GM_setClipboard" grant, both declared in build.c. */
static void mujs_native_action(js_State *J) {
    if (mujs__maybe_unbind_marker(J)) return;
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");

    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->action, sizeof(b->action), "action");

    if (js_isstring(J, 2)) {
        const char *dir = js_tostring(J, 2);
        if (strcmp(dir, "close") != 0 && strcmp(dir, "reload") != 0)
            mujs__reject(J, "action() must be close, reload, or yankurl(...)");
        snprintf(b->kind, sizeof(b->kind), "action");
        snprintf(b->dir, sizeof(b->dir), "%s", dir);

    } else if (js_isobject(J, 2)) {
        js_getproperty(J, 2, "$type");
        int is_yank = js_isstring(J, -1) && strcmp(js_tostring(J, -1), "__muyankurl__") == 0;
        js_pop(J, 1);
        if (!is_yank) mujs__reject(J, "action() must be close, reload, or yankurl(...)");

        snprintf(b->kind, sizeof(b->kind), "yankurl");

        js_getproperty(J, 2, "hasTarget");
        b->has_target = js_toboolean(J, -1);
        js_pop(J, 1);

        if (b->has_target) {
            js_getproperty(J, 2, "targetKind");
            snprintf(b->target_kind, sizeof(b->target_kind), "%s", js_tostring(J, -1));
            js_pop(J, 1);
            js_getproperty(J, 2, "targetValue");
            snprintf(b->value, sizeof(b->value), "%s", js_tostring(J, -1));
            js_pop(J, 1);
            js_getproperty(J, 2, "targetDir");
            snprintf(b->dir, sizeof(b->dir), "%s", js_tostring(J, -1));
            js_pop(J, 1);
            js_getproperty(J, 2, "targetLoop");
            snprintf(b->loopname, sizeof(b->loopname), "%s", js_tostring(J, -1));
            js_pop(J, 1);
            js_getproperty(J, 2, "targetSelLoops");
            int n = js_getlength(J, -1);
            for (int i = 0; i < n && i < MUJS_MAX_SEL_LOOPS; i++) {
                js_getindex(J, -1, i);
                snprintf(b->sel_loops[i], sizeof(b->sel_loops[i]), "%s", js_tostring(J, -1));
                js_pop(J, 1);
            }
            b->sel_loop_count = n;
            js_pop(J, 1); /* targetSelLoops array */
        }
    } else {
        mujs__reject(J, "action() must be close, reload, or yankurl(...)");
    }
    js_pushundefined(J);
}

/* root(key) -- jumps to the current site's origin root, e.g.
 * https://example.com/a/b/c -> https://example.com/ */
static void mujs_native_root(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "root");
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->kind, sizeof(b->kind), "root");
    js_pushundefined(J);
}

/* branch(key) -- goes up one path segment, e.g.
 * https://example.com/a/b/c -> https://example.com/a/b */
static void mujs_native_branch(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "branch");
    snprintf(b->keys, sizeof(b->keys), "%s", js_tostring(J, 1));
    snprintf(b->kind, sizeof(b->kind), "branch");
    js_pushundefined(J);
}

static void mujs_native_focus(js_State *J)       { mujs__bind(J, "focus"); }
static void mujs_native_click(js_State *J)       { mujs__bind(J, "click"); }
static void mujs_native_longpress(js_State *J)   { mujs__bind(J, "longpress"); }
static void mujs_native_doubleclick(js_State *J) { mujs__bind(J, "doubleclick"); }
static void mujs_native_opennew(js_State *J)     { mujs__bind(J, "opennew"); }

/* off(click(key)) -- explicitly unbinds `key` on THIS site, even if a
 * universal site (or the built-in gi/gI defaults) would otherwise fill
 * it in. Takes the marker produced by calling focus/click/longpress/
 * doubleclick/opennew with just a key and no target. */
/* off(click(key)) -- OR off("key") directly, for single-argument
 * declarators like root()/branch() that have no "no target given"
 * signal to detect in the first place. Either form ends up identical:
 * an explicit "unbind" entry for that key on this site. */
static void mujs_native_off(js_State *J) {
    mujs_site_t *s = &mujs__ctx(J)->site;
    const char *keys;

    if (js_isstring(J, 1)) {
        keys = js_tostring(J, 1);
    } else if (js_isobject(J, 1)) {
        js_getproperty(J, 1, "$type");
        int ok = js_isstring(J, -1) && strcmp(js_tostring(J, -1), "__muunbind__") == 0;
        js_pop(J, 1);
        if (!ok) mujs__reject(J, "off() expects e.g. click(key) with no target, or a plain key string");
        js_getproperty(J, 1, "keys");
        keys = js_tostring(J, -1);
    } else {
        mujs__reject(J, "off() expects e.g. click(key) with no target, or a plain key string");
        return; /* unreachable -- mujs__reject longjmps out */
    }

    if (s->binding_count >= MUJS_MAX_BINDINGS) mujs__reject(J, "too many bindings (raise MUJS_MAX_BINDINGS)");
    mujs_binding_t *b = &s->bindings[s->binding_count++];
    snprintf(b->action, sizeof(b->action), "off");
    snprintf(b->keys, sizeof(b->keys), "%s", keys);
    snprintf(b->kind, sizeof(b->kind), "off");
    js_pushundefined(J);
}

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
    js_newcfunction(J, mujs_native_scroll, "scroll", 3);         js_setglobal(J, "scroll");
    js_newcfunction(J, mujs_native_navigate, "navigate", 2);     js_setglobal(J, "navigate");
    js_newcfunction(J, mujs_native_action, "action", 2);         js_setglobal(J, "action");
    js_newcfunction(J, mujs_native_focus, "focus", 2);           js_setglobal(J, "focus");
    js_newcfunction(J, mujs_native_click, "click", 2);           js_setglobal(J, "click");
    js_newcfunction(J, mujs_native_longpress, "longpress", 2);   js_setglobal(J, "longpress");
    js_newcfunction(J, mujs_native_doubleclick, "doubleclick", 2); js_setglobal(J, "doubleclick");
    js_newcfunction(J, mujs_native_opennew, "opennew", 2);       js_setglobal(J, "opennew");
    js_newcfunction(J, mujs_native_off, "off", 1);               js_setglobal(J, "off");
    js_newcfunction(J, mujs_native_yankurl, "yankurl", 1);       js_setglobal(J, "yankurl");
    js_newcfunction(J, mujs_native_root, "root", 1);             js_setglobal(J, "root");
    js_newcfunction(J, mujs_native_branch, "branch", 1);         js_setglobal(J, "branch");

    /* `selected(...)` -- a call (not a bare value): selected() means
     * every loop's current cursor item; selected("MSG","RESULT") means
     * just those named loops' current items. Variadic -- declared
     * length 0, actual arg count read via js_gettop at call time. */
    js_newcfunction(J, mujs_native_selected, "selected", 0);
    js_setglobal(J, "selected");
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
        } else if (strcmp(b->kind, "scroll") == 0) {
            n += snprintf(out + n, cap - n, ", dir: ");
            mujs__json_escape(out, cap, &n, b->dir);
            n += snprintf(out + n, cap - n, ", amount: %s", b->value); /* a real number, not a string */
        } else if (strcmp(b->kind, "navigate") == 0) {
            n += snprintf(out + n, cap - n, ", dir: ");
            mujs__json_escape(out, cap, &n, b->dir);
        } else if (strcmp(b->kind, "action") == 0) {
            n += snprintf(out + n, cap - n, ", dir: ");
            mujs__json_escape(out, cap, &n, b->dir);
        } else if (strcmp(b->kind, "function") == 0) {
            n += snprintf(out + n, cap - n, ", value: (%s)", b->value); /* a REAL function, not a string */
        } else if (strcmp(b->kind, "goto") == 0) {
            n += snprintf(out + n, cap - n, ", dir: ");
            mujs__json_escape(out, cap, &n, b->dir);
            n += snprintf(out + n, cap - n, ", loop: ");
            mujs__json_escape(out, cap, &n, b->loopname);
        } else if (strcmp(b->kind, "selected") == 0) {
            n += snprintf(out + n, cap - n, ", loops: [");
            for (int li = 0; li < b->sel_loop_count; li++) {
                if (li) { out[n++] = ','; out[n++] = ' '; }
                mujs__json_escape(out, cap, &n, b->sel_loops[li]);
            }
            n += snprintf(out + n, cap - n, "]");
        } else if (strcmp(b->kind, "yankurl") == 0) {
            n += snprintf(out + n, cap - n, ", hasTarget: %s", b->has_target ? "true" : "false");
            if (b->has_target) {
                n += snprintf(out + n, cap - n, ", targetKind: ");
                mujs__json_escape(out, cap, &n, b->target_kind);
                if (strcmp(b->target_kind, "selector") == 0 || strcmp(b->target_kind, "function") == 0) {
                    n += snprintf(out + n, cap - n, ", targetValue: ");
                    /* selector is a string; function is a real captured
                     * function -- both cases were already validated by
                     * mujs__resolve_target, so embed accordingly. */
                    if (strcmp(b->target_kind, "function") == 0)
                        n += snprintf(out + n, cap - n, "(%s)", b->value);
                    else
                        mujs__json_escape(out, cap, &n, b->value);
                } else if (strcmp(b->target_kind, "goto") == 0) {
                    n += snprintf(out + n, cap - n, ", targetDir: ");
                    mujs__json_escape(out, cap, &n, b->dir);
                    n += snprintf(out + n, cap - n, ", targetLoop: ");
                    mujs__json_escape(out, cap, &n, b->loopname);
                } else if (strcmp(b->target_kind, "selected") == 0) {
                    n += snprintf(out + n, cap - n, ", targetSelLoops: [");
                    for (int li = 0; li < b->sel_loop_count; li++) {
                        if (li) { out[n++] = ','; out[n++] = ' '; }
                        mujs__json_escape(out, cap, &n, b->sel_loops[li]);
                    }
                    n += snprintf(out + n, cap - n, "]");
                }
            }
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
    char *file_raw = build__read_file(path, &len);
    char *raw = mujs__apply_aliases(file_raw); /* alias.js substitutions, before anything else */
    free(file_raw);

    mujs_dsl_t dsl;
    memset(&dsl, 0, sizeof(dsl));
    dsl.J = js_newstate(NULL, NULL, JS_STRICT);
    if (!dsl.J) { fprintf(stderr, "mujscompiler: js_newstate failed\n"); exit(1); }
    js_setcontext(dsl.J, &dsl);
    mujs__register_natives(dsl.J);

    if (js_dostring(dsl.J, MUJS_PRELUDE)) {
        fprintf(stderr, "mujscompiler: internal error loading prelude:\n  %s\n", js_trystring(dsl.J, -1, "error"));
        exit(1);
    }

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
