# phpc — PHP-to-native compiler (proof of concept)

> **⚠️ DO NOT USE THIS IN PRODUCTION. DO NOT USE THIS IN STAGING. DO NOT USE THIS ANYWHERE NEAR REAL CODE.**
>
> This is a proof-of-concept research project. It compiles one specific PHP snippet and nothing else. It will silently produce wrong output, crash, or refuse to compile for virtually any other PHP file. There is no type system, no error handling, no garbage collection, no standard library, no object support, no exception support, no reference counting, and no PHP compatibility of any kind. The only reason it exists is to show that the pipeline — PHP opcodes → LLVM IR → native binary — is viable in principle. Treat it as a demo, not a tool.

---

## What it does

`phpc` takes a PHP source file, embeds the Zend engine to compile it to VM opcodes, then translates those opcodes to LLVM IR and emits a native object file. The result is a self-contained binary with no PHP runtime dependency — only libc.

For the included `test.php`:

```php
<?php

function p($s)
{
    echo $s . "\n";
}

for ($x = 0; $x < 10; $x++) {
    if ($x % 2 == 0) {
        p("Hello world $x!");
    }
}
```

It produces a 13 KB binary that prints:

```
Hello world 0!
Hello world 2!
Hello world 4!
Hello world 6!
Hello world 8!
```

## Requirements

Fedora 43 (or similar). You need:

```
sudo dnf install -y php-devel php-embedded php-opcache clang llvm-devel
```

- PHP 8.4 with the embed SAPI (`libphp.so`)
- LLVM 21 (`llvm-devel`, `clang`)
- A C compiler (`clang`)

## Build

```bash
make
```

This produces the `phpc` binary.

## Usage

```bash
# Compile a PHP file to output.o and show LLVM IR
./phpc test.php

# Link the object file into a standalone binary
clang output.o -o a.out

# Run it — no PHP needed
./a.out
```

Or use the Makefile shortcut which does all three:

```bash
make test
```

## Verify there is no PHP dependency

```bash
nm a.out | grep -i php    # should print nothing
ldd a.out                 # should show only libc.so
```

## How it works

```
test.php
   │
   ▼  libphp.so (Zend embed SAPI)
zend_compile_file()  →  zend_op_array
   │
   ├── Phase 1: dump opcodes to stdout (src/dump.c)
   └── Phase 2: LLVM codegen (src/codegen.c)
         ├── Pass A — collect block leaders, create BasicBlocks
         └── Pass B — emit opcodes
               ASSIGN        → alloca + store
               JMP/JMPZ/JMPNZ → br / condBr
               MOD/IS_EQUAL/IS_SMALLER → srem / icmp
               PRE_INC       → load + add + store
               ROPE_INIT/ADD/END → snprintf into stack buffer
               INIT_FCALL/SEND_VAL/DO_UCALL → call p(buf)
               RETURN        → ret i32 0
   │
   ▼  output.o  →  clang output.o -o a.out
```

The Zend engine is used only at compile time to parse PHP and produce VM opcodes. It is not linked into the output binary. The output binary depends on nothing except libc.

## What is actually supported

The following opcodes, and only these:

| Opcode | Lowering |
|---|---|
| `ZEND_ASSIGN` | `alloca` + `store` (integer CVs only) |
| `ZEND_MOD` | `srem i64` |
| `ZEND_IS_EQUAL` | `icmp eq i64` |
| `ZEND_IS_SMALLER` | `icmp slt i64` |
| `ZEND_JMP` | unconditional `br` |
| `ZEND_JMPZ` | `br i1` (jump if zero) |
| `ZEND_JMPNZ` | `br i1` (jump if not zero) |
| `ZEND_PRE_INC` | `load` + `add` + `store` |
| `ZEND_ROPE_INIT/ADD/END` | `snprintf` into a stack buffer |
| `ZEND_INIT_FCALL` + `ZEND_SEND_VAL` + `ZEND_DO_UCALL` | direct `call` |
| `ZEND_RECV` | store argument into CV alloca |
| `ZEND_RETURN` | `ret` |
| `ZEND_ECHO` + `ZEND_CONCAT("\n")` | `puts` |

Everything else is silently ignored, which will produce incorrect binaries without warning.

## What is not supported (partial list)

Arrays, objects, classes, traits, interfaces, exceptions, closures, generators, references, type coercion, string functions, math functions, superglobals, `include`/`require`, multiple files, floating point variables, boolean variables, null, mixed types, dynamic function calls, variadic functions, recursion (stack overflow risk), and all of the PHP standard library.

## Project structure

```
phpc/
├── src/
│   ├── main.c      — embed SAPI init, compile PHP, drive phases
│   ├── dump.c/h    — Phase 1: print VM opcodes for inspection
│   └── codegen.c/h — Phase 2: translate opcodes to LLVM IR
├── Makefile
└── test.php        — the one PHP file this tool can compile
```

## License

Do whatever you want with it. Just don't ship it.
