#define OUTFILE "dist/granite.user.js"
#define BUILD_WITH_MUJS
#include "build.h"
#include "mujscompiler.h"

#define NAME        "granite"
#define NAMESPACE   "granite"
#define DESCRIPTION "Persite Keybinding Program"

listmatch(
    "*://*/*",
    );

listgrant(
    "window.close"
    );

listextra(
    { "//NAME", "//Description" },
    );

listorder(
    "src/start.js",
    "src/core.js",
    "src/end.js",
    );

declaremeta(
    .name = NAME,
    .namespace_ = NAMESPACE,
    .description = DESCRIPTION,
    .match = MATCH, .match_count = MATCH_COUNT,
    .grant = GRANT, .grant_count = GRANT_COUNT,
    .run_at = "document-start",
    .extra = EXTRA, .extra_count = EXTRA_COUNT,
);

int main(void) {
    const char *standalone_files[] = { "src/core.js" };
    mujs_check_all(standalone_files, 1, "src/");

    build_t b;
    build_init(&b, NULL, "__GRANITE_VERSION__");
    build_userscript_header(&b, &META);

    build_add(&b, "src/start.js", "src/");
    build_add(&b, "src/core.js", "src/");

    mujs_compile_sites_dir(&b, "src/sites");

    build_add(&b, "src/end.js", "src/");

    mujs_check_bundle(&b);
    build_finish(&b, NULL);
    return 0;
}
