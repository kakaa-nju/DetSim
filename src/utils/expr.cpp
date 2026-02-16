/*
 * expr.cpp - Expression evaluation using lex/yacc parser
 * 
 * This module provides expression evaluation for the debugger.
 * It uses a lex/yacc-based parser to support complex C expressions.
 */

#include "common.h"
#include "debug.h"
#include "guest.h"
#include "monitor.h"
#include "dwarf_info.h"
#include "expr_ast.hpp"
#include <cctype>
#include <string>
#include <vector>
#include <memory>
#include <sys/ptrace.h>
#include <cerrno>
#include <cstring>

/* Initialize parser globals from system state */
static void init_parser_globals() {
    static bool initialized = false;
    if (!initialized) {
        g_num_processes = NP;
        g_ptmc_state.pids = ptmc_state.pids;
        initialized = true;
    }
}

/* Get current process ID for expression evaluation */
static int get_current_pid() {
    int cursor = ptmc_state.cursor;
    if (cursor < 0 || cursor >= NP) cursor = 0;
    return ptmc_state.pids[cursor];
}

/* Evaluate expression and return value
 * Supports full C expressions including:
 * - Variables: g_point, counter
 * - Member access: g_point.x, ptr->field
 * - Array index: arr[5], g_line.points[2]
 * - Arithmetic: +, -, *, /, %
 * - Comparison: ==, !=, <, >, <=, >=
 * - Logical: &&, ||, !
 * - Bitwise: &, |, ^, <<, >>
 * - Address/deref: &var, *ptr
 * - sizeof: sizeof(point), sizeof(int)
 * - offsetof: offsetof(point, x)
 * - Process qualified: tracee0(g_point.x)
 */
long expr(const char *e, bool *success) {
    *success = false;
    if (!e || !*e) return 0;
    
    init_parser_globals();
    
    ExprNode* ast = parse_expression(e);
    if (!ast) {
        fprintf(stderr, "Parse error: failed to parse expression '%s'\n", e);
        return 0;
    }
    
    int pid = get_current_pid();
    bool ok = true;
    EvalResult result = ast->eval(pid, ok);
    
    delete ast;
    
    if (!ok) {
        fprintf(stderr, "Evaluation error: failed to evaluate expression '%s'\n", e);
        return 0;
    }
    
    *success = true;
    return result.as_value();
}

/* Print expression result in GDB style
 * For structs/arrays, prints detailed formatted output
 */
void expr_print(const char *e) {
    if (!e || !*e) return;
    
    init_parser_globals();
    
    int pid = get_current_pid();
    
    ExprNode* ast = parse_expression(e);
    if (!ast) {
        printf("Error parsing expression: %s\n", e);
        return;
    }
    
    /* Try to get address and type for detailed printing */
    bool ok = true;
    EvalResult result = ast->eval_address(pid, ok);
    
    if (ok) {
        /* We have an address - print with type info */
        uint64_t addr = result.as_address();
        const std::string& type_name = result.type_name;
        
        if (!type_name.empty()) {
            /* Get type info for detailed printing */
            type_info info = dwarf_get_type_info(type_name.c_str());
            
            if (info.is_struct) {
                printf("%s = {\n", e);
                for (const auto& m : info.members) {
                    /* Read member value */
                    uintptr_t maddr = addr + m.offset;
                    long val = 0;
                    errno = 0;
                    val = ptrace(PTRACE_PEEKDATA, pid, maddr, nullptr);
                    
                    /* Truncate based on member size */
                    if (m.size == 1) val = (int8_t)val;
                    else if (m.size == 2) val = (int16_t)val;
                    else if (m.size == 4) val = (int32_t)val;
                    
                    printf("  %s = %ld,\n", m.name.c_str(), val);
                }
                printf("}\n");
                delete ast;
                return;
            } else if (info.is_array) {
                /* Print array elements */
                int elem_size = info.size / (info.array_elements > 0 ? info.array_elements : 1);
                int print_count = info.array_elements > 10 ? 10 : info.array_elements;
                
                printf("%s = [", e);
                for (int i = 0; i < print_count; i++) {
                    uintptr_t eaddr = addr + i * elem_size;
                    long val = ptrace(PTRACE_PEEKDATA, pid, eaddr, nullptr);
                    if (i > 0) printf(", ");
                    printf("%ld", val);
                }
                if (info.array_elements > 10) printf(", ...");
                printf("]\n");
                delete ast;
                return;
            } else if (info.is_pointer) {
                /* Print pointer with dereference hint */
                printf("(%s) 0x%lx\n", type_name.c_str(), (unsigned long)addr);
                delete ast;
                return;
            }
        }
        
        /* Default: print value at address */
        long val = ptrace(PTRACE_PEEKDATA, pid, addr, nullptr);
        /* Truncate based on type size */
        if (!type_name.empty()) {
            size_t type_size = dwarf_type_size(type_name.c_str());
            if (type_size == 1) val = (int8_t)val;
            else if (type_size == 2) val = (int16_t)val;
            else if (type_size == 4) val = (int32_t)val;
        }
        printf("%ld\n", val);
    } else {
        /* Just a value */
        result = ast->eval(pid, ok);
        if (ok) {
            printf("%ld\n", result.as_value());
        } else {
            printf("Error evaluating expression\n");
        }
    }
    
    delete ast;
}

/* Dummy init function for compatibility */
void init_regex() {
    /* Nothing to do - parser is self-initializing */
}
