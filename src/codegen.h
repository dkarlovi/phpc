#pragma once

#include <Zend/zend_compile.h>

/* Translate op_array (and user functions it references) into a native binary.
   Returns 0 on success. */
int codegen(const zend_op_array *main_op_array, const char *out_obj);
