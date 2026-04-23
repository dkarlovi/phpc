#include "dump.h"

#include <Zend/zend_vm_opcodes.h>
#include <Zend/zend_types.h>
#include <Zend/zend_compile.h>
#include <stdio.h>

static void print_const(const zend_op *op, const znode_op *node)
{
    const zval *cv = RT_CONSTANT(op, *node);
    switch (Z_TYPE_P(cv)) {
        case IS_LONG:   printf("LONG(%ld)",   Z_LVAL_P(cv)); break;
        case IS_DOUBLE: printf("DOUBLE(%f)",  Z_DVAL_P(cv)); break;
        case IS_STRING: printf("STR(\"%s\")", Z_STRVAL_P(cv)); break;
        case IS_NULL:   printf("NULL"); break;
        case IS_TRUE:   printf("TRUE"); break;
        case IS_FALSE:  printf("FALSE"); break;
        default:        printf("CONST(type=%d)", Z_TYPE_P(cv)); break;
    }
}

static void print_operand(const zend_op_array *oa, const zend_op *op,
                          const znode_op *node, uint8_t type)
{
    switch (type) {
        case IS_CONST:   print_const(op, node); break;
        case IS_TMP_VAR: printf("T%lu", (node->var - sizeof(zend_execute_data)) / sizeof(zval)); break;
        case IS_VAR:     printf("V%lu", (node->var - sizeof(zend_execute_data)) / sizeof(zval)); break;
        case IS_CV: {
            uint32_t idx = (node->var - sizeof(zend_execute_data)) / sizeof(zval);
            if (idx < oa->last_var && oa->vars[idx]) {
                printf("$%s", ZSTR_VAL(oa->vars[idx]));
            } else {
                printf("CV%u", idx);
            }
            break;
        }
        case IS_UNUSED:  printf("_"); break;
        default:         printf("?%d", type); break;
    }
}

static size_t jmp_target(const zend_op_array *oa, const zend_op *op, const znode_op *node)
{
    return (size_t)(OP_JMP_ADDR(op, *node) - oa->opcodes);
}

void dump_op_array(const zend_op_array *oa, const char *label)
{
    printf("\n=== %s (%u ops) ===\n", label, oa->last);
    for (uint32_t i = 0; i < oa->last; i++) {
        const zend_op *op = &oa->opcodes[i];
        printf("  %3u  %-28s  ", i, zend_get_opcode_name(op->opcode));
        print_operand(oa, op, &op->result, op->result_type);
        printf("  =  ");
        print_operand(oa, op, &op->op1, op->op1_type);
        printf("  ,  ");
        print_operand(oa, op, &op->op2, op->op2_type);

        switch (op->opcode) {
            case ZEND_JMP:
                printf("  -> %zu", jmp_target(oa, op, &op->op1));
                break;
            case ZEND_JMPZ:
            case ZEND_JMPNZ:
            case ZEND_JMPZ_EX:
            case ZEND_JMPNZ_EX:
                printf("  -> %zu", jmp_target(oa, op, &op->op2));
                break;
        }

        printf("\n");
    }
}
