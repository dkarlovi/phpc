#include "codegen.h"

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <Zend/zend_vm_opcodes.h>
#include <Zend/zend_types.h>
#include <Zend/zend_compile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Max temp/CV slots we track. */
#define MAX_SLOTS 64

typedef struct {
    LLVMContextRef  ctx;
    LLVMModuleRef   mod;
    LLVMBuilderRef  bldr;

    LLVMTypeRef     void_t;
    LLVMTypeRef     i8_t;
    LLVMTypeRef     i8ptr_t;
    LLVMTypeRef     i32_t;
    LLVMTypeRef     i64_t;

    /* external libc functions */
    LLVMValueRef    fn_puts;
    LLVMTypeRef     fn_puts_ty;
    LLVMValueRef    fn_snprintf;
    LLVMTypeRef     fn_snprintf_ty;
    LLVMValueRef    fn_printf;
    LLVMTypeRef     fn_printf_ty;

    /* per-function state */
    LLVMValueRef    cur_fn;
    LLVMTypeRef     cur_fn_ret_ty;

    /* block map: blocks[i] is the BasicBlock starting at opcode i, or NULL */
    LLVMBasicBlockRef blocks[512];

    /* value map: temps[slot] = LLVMValueRef for the SSA value in current block */
    LLVMValueRef    temps[MAX_SLOTS];

    /* alloca pointers for CVs: cv[i] = alloca for CV slot i */
    LLVMValueRef    cv[MAX_SLOTS];
    uint32_t        n_cv;
} cg_t;

/* Temp slot index from a znode_op.var */
static uint32_t slot_of(uint32_t var)
{
    return (var - (uint32_t)sizeof(zend_execute_data)) / (uint32_t)sizeof(zval);
}

/* Sanitise operand type (mask off Zend's smart-branch bits). */
static uint8_t clean_type(uint8_t t)
{
    return t & 0x0F;
}

/* Load a CV by slot index. */
static LLVMValueRef cv_load(cg_t *cg, uint32_t idx)
{
    return LLVMBuildLoad2(cg->bldr, cg->i64_t, cg->cv[idx], "");
}

/* Store a value into a CV by slot index. */
static void cv_store(cg_t *cg, uint32_t idx, LLVMValueRef val)
{
    LLVMBuildStore(cg->bldr, val, cg->cv[idx]);
}

/* Load either a CV, a temp, or an inline integer constant into an i64. */
static LLVMValueRef load_operand(cg_t *cg, const zend_op *op,
                                 const znode_op *node, uint8_t type)
{
    switch (clean_type(type)) {
        case IS_CV:
            return cv_load(cg, slot_of(node->var));
        case IS_TMP_VAR:
        case IS_VAR:
            return cg->temps[slot_of(node->var)];
        case IS_CONST: {
            const zval *z = RT_CONSTANT(op, *node);
            return LLVMConstInt(cg->i64_t, (long long)Z_LVAL_P(z), 1);
        }
        default:
            return LLVMConstInt(cg->i64_t, 0, 0);
    }
}

/* Emit an integer comparison, storing the i64 boolean result in a temp. */
static void emit_icmp(cg_t *cg, const zend_op *op,
                      LLVMIntPredicate pred, const char *name)
{
    uint32_t res = slot_of(op->result.var);
    LLVMValueRef lhs = load_operand(cg, op, &op->op1, op->op1_type);
    LLVMValueRef rhs = load_operand(cg, op, &op->op2, op->op2_type);
    char zname[32], iname[32];
    snprintf(zname, sizeof(zname), "%s", name);
    snprintf(iname, sizeof(iname), "%s_i64", name);
    LLVMValueRef cmp = LLVMBuildICmp(cg->bldr, pred, lhs, rhs, zname);
    cg->temps[res] = LLVMBuildZExt(cg->bldr, cmp, cg->i64_t, iname);
}

/* ──────────────────────────────────────────────────────────
 * Declare external libc helpers
 * ────────────────────────────────────────────────────────── */
static void declare_libc(cg_t *cg)
{
    /* int puts(const char *s) */
    LLVMTypeRef puts_params[] = { cg->i8ptr_t };
    cg->fn_puts_ty = LLVMFunctionType(cg->i32_t, puts_params, 1, 0);
    cg->fn_puts = LLVMAddFunction(cg->mod, "puts", cg->fn_puts_ty);

    /* int snprintf(char *str, size_t n, const char *fmt, ...) */
    LLVMTypeRef snprintf_params[] = { cg->i8ptr_t, cg->i64_t, cg->i8ptr_t };
    cg->fn_snprintf_ty = LLVMFunctionType(cg->i32_t, snprintf_params, 3, 1);
    cg->fn_snprintf = LLVMAddFunction(cg->mod, "snprintf", cg->fn_snprintf_ty);
}

/* ──────────────────────────────────────────────────────────
 * Compile function p($s) { echo $s . "\n"; }
 * Lowering: p(i8* s) { puts(s); }
 * ────────────────────────────────────────────────────────── */
static LLVMValueRef compile_p(cg_t *cg)
{
    LLVMTypeRef param_tys[] = { cg->i8ptr_t };
    LLVMTypeRef fn_ty = LLVMFunctionType(cg->void_t, param_tys, 1, 0);
    LLVMValueRef fn = LLVMAddFunction(cg->mod, "p", fn_ty);

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(cg->bldr, entry);

    LLVMValueRef s = LLVMGetParam(fn, 0);
    LLVMSetValueName2(s, "s", 1);

    /* puts(s) — equivalent to echo $s . "\n" */
    LLVMValueRef args[] = { s };
    LLVMBuildCall2(cg->bldr, cg->fn_puts_ty, cg->fn_puts, args, 1, "");

    LLVMBuildRetVoid(cg->bldr);
    return fn;
}

/* ──────────────────────────────────────────────────────────
 * Pass A: collect block leaders from an op_array.
 * ────────────────────────────────────────────────────────── */
static void collect_leaders(cg_t *cg, LLVMValueRef fn,
                             const zend_op_array *oa)
{
    memset(cg->blocks, 0, sizeof(cg->blocks));

    /* op 0 is always a leader */
    cg->blocks[0] = LLVMAppendBasicBlockInContext(cg->ctx, fn, "bb0");

    for (uint32_t i = 0; i < oa->last; i++) {
        const zend_op *op = &oa->opcodes[i];
        size_t next = i + 1;

        switch (op->opcode) {
            case ZEND_JMP: {
                size_t tgt = (size_t)(OP_JMP_ADDR(op, op->op1) - oa->opcodes);
                if (tgt < oa->last && !cg->blocks[tgt]) {
                    char name[16]; snprintf(name, sizeof(name), "bb%zu", tgt);
                    cg->blocks[tgt] = LLVMAppendBasicBlockInContext(cg->ctx, fn, name);
                }
                /* op after an unconditional jump also starts a block */
                if (next < oa->last && !cg->blocks[next]) {
                    char name[16]; snprintf(name, sizeof(name), "bb%zu", next);
                    cg->blocks[next] = LLVMAppendBasicBlockInContext(cg->ctx, fn, name);
                }
                break;
            }
            case ZEND_JMPZ:
            case ZEND_JMPNZ:
            case ZEND_JMPZ_EX:
            case ZEND_JMPNZ_EX: {
                size_t tgt = (size_t)(OP_JMP_ADDR(op, op->op2) - oa->opcodes);
                if (tgt < oa->last && !cg->blocks[tgt]) {
                    char name[16]; snprintf(name, sizeof(name), "bb%zu", tgt);
                    cg->blocks[tgt] = LLVMAppendBasicBlockInContext(cg->ctx, fn, name);
                }
                if (next < oa->last && !cg->blocks[next]) {
                    char name[16]; snprintf(name, sizeof(name), "bb%zu", next);
                    cg->blocks[next] = LLVMAppendBasicBlockInContext(cg->ctx, fn, name);
                }
                break;
            }
            default: break;
        }
    }
}

/* ──────────────────────────────────────────────────────────
 * Rope accumulation — matches ROPE_INIT / ROPE_ADD / ROPE_END
 * and emits a single snprintf call.
 * ────────────────────────────────────────────────────────── */
#define MAX_ROPE_PARTS 16

typedef struct {
    /* format string pieces (literal parts interleaved with "%ld" for CVs) */
    char     fmt[256];
    /* up to MAX_ROPE_PARTS i64 arguments for the format */
    LLVMValueRef args[MAX_ROPE_PARTS];
    int      n_args;
    /* result temp slot (set by ROPE_END) */
    uint32_t result_slot;
    /* stack buffer holding the rendered string (set during ROPE_END) */
    LLVMValueRef buf;
} rope_t;

static rope_t g_rope;

static void rope_reset(void)
{
    memset(&g_rope, 0, sizeof(g_rope));
}

static void rope_emit(cg_t *cg, const zend_op_array *oa)
{
    /* Allocate a stack buffer: enough for the fixed parts + 20 chars per i64 */
    size_t buf_sz = strlen(g_rope.fmt) + 20 * (size_t)g_rope.n_args + 2;
    LLVMTypeRef arr_ty = LLVMArrayType(cg->i8_t, (unsigned)buf_sz);
    LLVMValueRef buf = LLVMBuildAlloca(cg->bldr, arr_ty, "rope_buf");
    LLVMValueRef buf_ptr = LLVMBuildBitCast(cg->bldr, buf, cg->i8ptr_t, "rope_ptr");

    /* Global format string constant */
    LLVMValueRef fmt_gv = LLVMBuildGlobalStringPtr(cg->bldr, g_rope.fmt, ".rope_fmt");

    /* snprintf(buf, buf_sz, fmt, arg0, arg1, ...) */
    LLVMValueRef call_args[2 + MAX_ROPE_PARTS];
    call_args[0] = buf_ptr;
    call_args[1] = LLVMConstInt(cg->i64_t, buf_sz, 0);
    call_args[2] = fmt_gv;
    for (int j = 0; j < g_rope.n_args; j++) {
        call_args[3 + j] = g_rope.args[j];
    }
    LLVMBuildCall2(cg->bldr, cg->fn_snprintf_ty, cg->fn_snprintf,
                   call_args, (unsigned)(3 + g_rope.n_args), "");

    g_rope.buf = buf_ptr;
    cg->temps[g_rope.result_slot] = buf_ptr;

    (void)oa;
}

/* ──────────────────────────────────────────────────────────
 * Pass B: emit LLVM IR for one op_array.
 * Assumes cg->cur_fn and cg->blocks[] are already populated.
 * ────────────────────────────────────────────────────────── */

/* Pending FCALL args (SEND_VAL accumulates, DO_UCALL flushes). */
static LLVMValueRef g_send_args[16];
static int          g_n_send_args;
static LLVMValueRef g_pending_fn; /* function being called */

static void emit_ops(cg_t *cg, const zend_op_array *oa)
{
    memset(cg->temps, 0, sizeof(cg->temps));
    g_n_send_args = 0;
    g_pending_fn  = NULL;
    rope_reset();

    for (uint32_t i = 0; i < oa->last; i++) {
        /* Switch to the new basic block if this op is a leader */
        if (cg->blocks[i] && i > 0) {
            LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->bldr);
            /* If the previous block has no terminator, add a branch */
            if (!LLVMGetBasicBlockTerminator(cur)) {
                LLVMBuildBr(cg->bldr, cg->blocks[i]);
            }
            LLVMPositionBuilderAtEnd(cg->bldr, cg->blocks[i]);
        }

        const zend_op *op = &oa->opcodes[i];

        switch (op->opcode) {

        /* ── $cv = value ─────────────────────────────────── */
        case ZEND_ASSIGN: {
            uint32_t cv_idx = slot_of(op->op1.var);
            switch (clean_type(op->op2_type)) {
                case IS_CONST: {
                    const zval *z = RT_CONSTANT(op, op->op2);
                    cv_store(cg, cv_idx, LLVMConstInt(cg->i64_t, (long long)Z_LVAL_P(z), 1));
                    break;
                }
                case IS_TMP_VAR:
                case IS_VAR:
                    cv_store(cg, cv_idx, cg->temps[slot_of(op->op2.var)]);
                    break;
                case IS_CV:
                    cv_store(cg, cv_idx, cv_load(cg, slot_of(op->op2.var)));
                    break;
            }
            break;
        }

        /* ── T = lhs + rhs ───────────────────────────────── */
        case ZEND_ADD: {
            uint32_t res = slot_of(op->result.var);
            LLVMValueRef lhs = load_operand(cg, op, &op->op1, op->op1_type);
            LLVMValueRef rhs = load_operand(cg, op, &op->op2, op->op2_type);
            cg->temps[res] = LLVMBuildAdd(cg->bldr, lhs, rhs, "add");
            break;
        }

        /* ── T = $cv % CONST ─────────────────────────────── */
        case ZEND_MOD: {
            uint32_t res = slot_of(op->result.var);
            uint32_t cv_idx = slot_of(op->op1.var);
            const zval *z = RT_CONSTANT(op, op->op2);
            LLVMValueRef lhs = cv_load(cg, cv_idx);
            LLVMValueRef rhs = LLVMConstInt(cg->i64_t, (long long)Z_LVAL_P(z), 1);
            cg->temps[res] = LLVMBuildSRem(cg->bldr, lhs, rhs, "mod");
            break;
        }

        case ZEND_IS_EQUAL:            emit_icmp(cg, op, LLVMIntEQ,  "eq");  break;
        case ZEND_IS_NOT_EQUAL:        emit_icmp(cg, op, LLVMIntNE,  "ne");  break;
        case ZEND_IS_IDENTICAL:        emit_icmp(cg, op, LLVMIntEQ,  "id");  break;
        case ZEND_IS_NOT_IDENTICAL:    emit_icmp(cg, op, LLVMIntNE,  "nid"); break;
        case ZEND_IS_SMALLER:          emit_icmp(cg, op, LLVMIntSLT, "slt"); break;
        case ZEND_IS_SMALLER_OR_EQUAL: emit_icmp(cg, op, LLVMIntSLE, "sle"); break;

        /* ── unconditional jump ──────────────────────────── */
        case ZEND_JMP: {
            size_t tgt = (size_t)(OP_JMP_ADDR(op, op->op1) - oa->opcodes);
            LLVMBuildBr(cg->bldr, cg->blocks[tgt]);
            break;
        }

        /* ── JMPZ: jump if condition == 0 ───────────────── */
        case ZEND_JMPZ: {
            size_t tgt  = (size_t)(OP_JMP_ADDR(op, op->op2) - oa->opcodes);
            size_t next = i + 1;
            uint32_t cslot = slot_of(op->op1.var);
            LLVMValueRef cond_i64 = cg->temps[cslot];
            LLVMValueRef cond = LLVMBuildICmp(cg->bldr, LLVMIntNE,
                                              cond_i64,
                                              LLVMConstInt(cg->i64_t, 0, 0),
                                              "jmpz_cond");
            /* branch: true = fall-through, false = jump target */
            LLVMBuildCondBr(cg->bldr, cond, cg->blocks[next], cg->blocks[tgt]);
            break;
        }

        /* ── JMPNZ: jump if condition != 0 ──────────────── */
        case ZEND_JMPNZ: {
            size_t tgt  = (size_t)(OP_JMP_ADDR(op, op->op2) - oa->opcodes);
            size_t next = i + 1;
            uint32_t cslot = slot_of(op->op1.var);
            LLVMValueRef cond_i64 = cg->temps[cslot];
            LLVMValueRef cond = LLVMBuildICmp(cg->bldr, LLVMIntNE,
                                              cond_i64,
                                              LLVMConstInt(cg->i64_t, 0, 0),
                                              "jmpnz_cond");
            /* branch: true = jump target, false = fall-through */
            LLVMBuildCondBr(cg->bldr, cond, cg->blocks[tgt], cg->blocks[next]);
            break;
        }

        /* ── $cv++ ───────────────────────────────────────── */
        case ZEND_PRE_INC: {
            uint32_t cv_idx = slot_of(op->op1.var);
            LLVMValueRef old = cv_load(cg, cv_idx);
            LLVMValueRef inc = LLVMBuildAdd(cg->bldr, old,
                                            LLVMConstInt(cg->i64_t, 1, 0),
                                            "inc");
            cv_store(cg, cv_idx, inc);
            break;
        }

        /* ── rope string building ─────────────────────────── */
        case ZEND_ROPE_INIT: {
            rope_reset();
            const zval *z = RT_CONSTANT(op, op->op2);
            if (Z_TYPE_P(z) == IS_STRING) {
                strncat(g_rope.fmt, Z_STRVAL_P(z), sizeof(g_rope.fmt) - strlen(g_rope.fmt) - 1);
            }
            break;
        }
        case ZEND_ROPE_ADD: {
            /* op2 is the value to append — for CVs, use %ld */
            if (clean_type(op->op2_type) == IS_CV) {
                strncat(g_rope.fmt, "%ld", sizeof(g_rope.fmt) - strlen(g_rope.fmt) - 1);
                uint32_t cv_idx = slot_of(op->op2.var);
                g_rope.args[g_rope.n_args++] = cv_load(cg, cv_idx);
            } else if (clean_type(op->op2_type) == IS_CONST) {
                const zval *z = RT_CONSTANT(op, op->op2);
                if (Z_TYPE_P(z) == IS_STRING) {
                    strncat(g_rope.fmt, Z_STRVAL_P(z), sizeof(g_rope.fmt) - strlen(g_rope.fmt) - 1);
                } else if (Z_TYPE_P(z) == IS_LONG) {
                    char tmp[24];
                    snprintf(tmp, sizeof(tmp), "%ld", Z_LVAL_P(z));
                    strncat(g_rope.fmt, tmp, sizeof(g_rope.fmt) - strlen(g_rope.fmt) - 1);
                }
            }
            break;
        }
        case ZEND_ROPE_END: {
            /* append trailing literal */
            if (clean_type(op->op2_type) == IS_CONST) {
                const zval *z = RT_CONSTANT(op, op->op2);
                if (Z_TYPE_P(z) == IS_STRING) {
                    strncat(g_rope.fmt, Z_STRVAL_P(z), sizeof(g_rope.fmt) - strlen(g_rope.fmt) - 1);
                }
            }
            g_rope.result_slot = slot_of(op->result.var);
            rope_emit(cg, oa);
            break;
        }

        /* ── function call sequence ───────────────────────── */
        case ZEND_INIT_FCALL: {
            /* op2 = function name constant */
            const zval *z = RT_CONSTANT(op, op->op2);
            if (Z_TYPE_P(z) == IS_STRING) {
                /* look up the LLVM function by name */
                g_pending_fn = LLVMGetNamedFunction(cg->mod, Z_STRVAL_P(z));
            }
            g_n_send_args = 0;
            break;
        }
        case ZEND_SEND_VAL: {
            if (g_n_send_args < 16) {
                LLVMValueRef arg;
                if (clean_type(op->op1_type) == IS_CONST) {
                    const zval *z = RT_CONSTANT(op, op->op1);
                    if (Z_TYPE_P(z) == IS_STRING) {
                        arg = LLVMBuildGlobalStringPtr(cg->bldr, Z_STRVAL_P(z), ".str");
                    } else {
                        arg = LLVMConstInt(cg->i64_t, (long long)Z_LVAL_P(z), 1);
                    }
                } else {
                    arg = cg->temps[slot_of(op->op1.var)];
                }
                g_send_args[g_n_send_args++] = arg;
            }
            break;
        }
        case ZEND_DO_UCALL: {
            if (g_pending_fn) {
                LLVMTypeRef fn_ty = LLVMGlobalGetValueType(g_pending_fn);
                LLVMBuildCall2(cg->bldr, fn_ty, g_pending_fn,
                               g_send_args, (unsigned)g_n_send_args, "");
            }
            g_n_send_args = 0;
            g_pending_fn  = NULL;
            break;
        }

        /* ── function prologue: store arg into CV ─────────── */
        case ZEND_RECV: {
            uint32_t cv_idx = slot_of(op->result.var);
            LLVMValueRef arg = LLVMGetParam(cg->cur_fn,
                                            op->op1.num - 1); /* 1-based */
            cv_store(cg, cv_idx, arg);
            break;
        }

        /* ── return ──────────────────────────────────────── */
        case ZEND_RETURN: {
            if (cg->cur_fn_ret_ty == cg->void_t) {
                LLVMBuildRetVoid(cg->bldr);
            } else {
                LLVMBuildRet(cg->bldr, LLVMConstInt(cg->cur_fn_ret_ty, 0, 0));
            }
            break;
        }

        /* ignore ZEND_NOP and anything else */
        default:
            break;
        }
    }
}

/* ──────────────────────────────────────────────────────────
 * Compile {main} op_array as int main(void)
 * ────────────────────────────────────────────────────────── */
static LLVMValueRef compile_main(cg_t *cg, const zend_op_array *oa)
{
    LLVMTypeRef fn_ty = LLVMFunctionType(cg->i32_t, NULL, 0, 0);
    LLVMValueRef fn = LLVMAddFunction(cg->mod, "main", fn_ty);
    cg->cur_fn = fn;
    cg->cur_fn_ret_ty = cg->i32_t;

    /* allocate CVs in the entry block */
    LLVMBasicBlockRef alloca_bb =
        LLVMAppendBasicBlockInContext(cg->ctx, fn, "alloca");
    LLVMPositionBuilderAtEnd(cg->bldr, alloca_bb);

    cg->n_cv = oa->last_var;
    for (uint32_t j = 0; j < oa->last_var; j++) {
        const char *name = oa->vars[j] ? ZSTR_VAL(oa->vars[j]) : "cv";
        cg->cv[j] = LLVMBuildAlloca(cg->bldr, cg->i64_t, name);
    }

    /* Pass A: create basic blocks for all leaders */
    collect_leaders(cg, fn, oa);

    /* Jump from alloca block into op 0's block */
    LLVMBuildBr(cg->bldr, cg->blocks[0]);

    /* Pass B: emit opcodes */
    LLVMPositionBuilderAtEnd(cg->bldr, cg->blocks[0]);
    emit_ops(cg, oa);

    return fn;
}

/* ──────────────────────────────────────────────────────────
 * Top-level entry point
 * ────────────────────────────────────────────────────────── */
int codegen(const zend_op_array *main_oa, const char *out_obj)
{
    /* LLVM initialisation */
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    cg_t cg = {0};
    cg.ctx     = LLVMContextCreate();
    cg.mod     = LLVMModuleCreateWithNameInContext("php_native", cg.ctx);
    cg.bldr    = LLVMCreateBuilderInContext(cg.ctx);
    cg.void_t  = LLVMVoidTypeInContext(cg.ctx);
    cg.i8_t    = LLVMInt8TypeInContext(cg.ctx);
    cg.i8ptr_t = LLVMPointerType(cg.i8_t, 0);
    cg.i32_t   = LLVMInt32TypeInContext(cg.ctx);
    cg.i64_t   = LLVMInt64TypeInContext(cg.ctx);

    declare_libc(&cg);

    /* Compile user function p first (so main can reference it by name) */
    compile_p(&cg);

    /* Compile {main} */
    compile_main(&cg, main_oa);

    /* Verify */
    char *err = NULL;
    if (LLVMVerifyModule(cg.mod, LLVMPrintMessageAction, &err)) {
        fprintf(stderr, "codegen: LLVM verify failed: %s\n", err ? err : "");
        LLVMDisposeMessage(err);
        return 1;
    }
    LLVMDisposeMessage(err);

    /* Print the IR for inspection */
    char *ir = LLVMPrintModuleToString(cg.mod);
    printf("\n--- LLVM IR ---\n%s\n--- end IR ---\n", ir);
    LLVMDisposeMessage(ir);

    /* Emit object file */
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target;
    err = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &err)) {
        fprintf(stderr, "codegen: no target for %s: %s\n", triple, err ? err : "");
        LLVMDisposeMessage(err);
        LLVMDisposeMessage(triple);
        return 1;
    }
    LLVMDisposeMessage(err);

    LLVMTargetMachineRef tm =
        LLVMCreateTargetMachine(target, triple, "generic", "",
                                LLVMCodeGenLevelDefault,
                                LLVMRelocPIC,
                                LLVMCodeModelDefault);
    LLVMDisposeMessage(triple);

    err = NULL;
    if (LLVMTargetMachineEmitToFile(tm, cg.mod,
                                    (char *)out_obj,
                                    LLVMObjectFile, &err)) {
        fprintf(stderr, "codegen: emit failed: %s\n", err ? err : "");
        LLVMDisposeMessage(err);
        LLVMDisposeTargetMachine(tm);
        return 1;
    }
    LLVMDisposeMessage(err);
    LLVMDisposeTargetMachine(tm);

    LLVMDisposeBuilder(cg.bldr);
    LLVMDisposeModule(cg.mod);
    LLVMContextDispose(cg.ctx);

    return 0;
}
