/* goober - the GooberBASIC command line tool */
#include "common.h"
#include "lexer.h"
#include "compiler.h"
#include "vm.h"

static void usage(void) {
    fprintf(stderr,
            "GooberBASIC %s\n"
            "usage: goober [options] program.gb\n"
            "  --check     compile only, report errors\n"
            "  --disasm    print the compiled bytecode\n"
            "  --version   print the version\n",
            GB_VERSION);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    bool check = false, disasm = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--disasm")) disasm = true;
        else if (!strcmp(argv[i], "--version")) { printf("GooberBASIC %s\n", GB_VERSION); return 0; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { usage(); return 2; }
        else if (!path) path = argv[i];
        else { usage(); return 2; }
    }
    if (!path) { usage(); return 2; }

    Program prog;
    memset(&prog, 0, sizeof prog);
    TokenList toks = {0};
    lex_file(path, &toks, &prog);
    compile_program(&toks, &prog);

    if (disasm) { disassemble(&prog); return 0; }
    if (check) { printf("%s: OK\n", path); return 0; }

    VM vm;
    vm_init(&vm, &prog);
    int code = vm_run(&vm);
    fflush(stdout);
    gfx_shutdown();
    vm_free(&vm);
    return code;
}
