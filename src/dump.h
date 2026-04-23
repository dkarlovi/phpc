#pragma once

#include <Zend/zend_compile.h>

void dump_op_array(const zend_op_array *op_array, const char *label);
