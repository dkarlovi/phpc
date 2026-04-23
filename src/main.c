#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sapi/embed/php_embed.h>
#include <Zend/zend_compile.h>
#include <Zend/zend_API.h>
#include <main/php_main.h>

#include "dump.h"
#include "codegen.h"

static void dump_user_functions(void)
{
    zend_string *name;
    zend_function *fn;

    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(function_table), name, fn) {
        if (fn->type == ZEND_USER_FUNCTION) {
            dump_op_array(&fn->op_array, ZSTR_VAL(name));
        }
    } ZEND_HASH_FOREACH_END();
}

static int run(const char *php_file)
{
    int rc = 1;

    zend_file_handle fh;
    zend_stream_init_filename(&fh, php_file);

    zend_op_array *main_oa = zend_compile_file(&fh, ZEND_REQUIRE);
    zend_destroy_file_handle(&fh);

    if (!main_oa) {
        fprintf(stderr, "phpc: compilation failed\n");
        return 1;
    }

    dump_op_array(main_oa, "{main}");
    dump_user_functions();

    rc = codegen(main_oa, "output.o");
    if (rc == 0) {
        fputs("\nphpc: linking...\n", stderr);
        int link_rc = system("clang output.o -o a.out");
        if (link_rc != 0) {
            fprintf(stderr, "phpc: link failed (exit %d)\n", link_rc);
            rc = 1;
        } else {
            fputs("phpc: done — ./a.out\n", stderr);
        }
    }

    destroy_op_array(main_oa);
    efree(main_oa);

    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: phpc <file.php>\n");
        return 1;
    }

    int rc = 1;

    php_embed_init(argc, argv);

    zend_first_try {
        rc = run(argv[1]);
    } zend_catch {
        fprintf(stderr, "phpc: zend exception\n");
    } zend_end_try();

    php_embed_shutdown();

    return rc;
}
