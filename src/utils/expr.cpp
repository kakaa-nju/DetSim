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

/* Check if expression starts with typeof */
static bool is_typeof_expr(const char* e) {
    while (*e && isspace(*e)) e++;
    return strncmp(e, "typeof", 6) == 0;
}

/* Forward declaration for recursive printing */
/* Forward declaration for mutual recursion */
static void print_value_recursive(int pid, const char* expr, uint64_t addr, 
                                   const std::string& type_name, int indent, 
                                   bool is_member = false, bool is_toplevel = true);

/* Print a single member value (with trailing comma) */
static void print_member_value(int pid, const char* name, uint64_t addr, 
                                const std::string& type_name, int indent) {
    print_value_recursive(pid, name, addr, type_name, indent, true, false);
}

/* Helper to read string from tracee memory */
static std::string read_tracee_string(int pid, uint64_t addr, size_t max_len = 64) {
    std::string result;
    result.reserve(max_len);
    for (size_t i = 0; i < max_len; i += 8) {
        long val = ptrace(PTRACE_PEEKDATA, pid, addr + i, nullptr);
        if (errno != 0) break;
        for (size_t j = 0; j < 8 && result.size() < max_len; j++) {
            char c = (val >> (j * 8)) & 0xFF;
            if (c == '\0') return result;
            result.push_back(c);
        }
    }
    if (result.size() >= max_len) result += "...";
    return result;
}

/* Check if type is char* (handle various spacing) */
static bool is_char_pointer(const std::string& type_name) {
    // Remove all spaces and compare
    std::string normalized;
    for (char c : type_name) {
        if (!isspace(c)) normalized += c;
    }
    return normalized == "char*";
}

/* Recursive value printer 
 * is_member: if true, print as struct member with name = value,\n format
 * is_toplevel: if true, this is the top-level expression
 */
static void print_value_recursive(int pid, const char* expr, uint64_t addr, 
                                   const std::string& type_name, int indent, 
                                   bool is_member, bool is_toplevel) {
    /* Print indentation for members */
    for (int i = 0; i < indent; i++) printf("  ");
    
    if (type_name.empty()) {
        long val = ptrace(PTRACE_PEEKDATA, pid, addr, nullptr);
        if (is_member) {
            printf("%s = %ld,\n", expr, val);
        } else {
            printf("%s = %ld\n", expr, val);
        }
        return;
    }
    
    type_info info = dwarf_get_type_info(type_name.c_str());
    if (info.is_struct) {
        /* Print struct */
        if (is_member) {
            printf("%s = {\n", expr);
        } else if (indent > 0) {
            printf("{\n");
        } else {
            printf("%s = {\n", expr);
        }
        for (const auto& m : info.members) {
            print_member_value(pid, m.name.c_str(), addr + m.offset, m.type_name, indent + 1);
        }
        for (int i = 0; i < indent; i++) printf("  ");
        if (is_member) {
            printf("},\n");
        } else {
            printf("}\n");
        }
    } else if (info.is_array) {
        /* Print array */
        if (is_member) {
            printf("%s = [\n", expr);
        } else if (indent > 0) {
            printf("[\n");
        } else {
            printf("%s = [\n", expr);
        }
        int elem_size = info.element_type.empty() ? 8 : dwarf_type_size(info.element_type.c_str());
        if (elem_size == 0) elem_size = 8;
        int print_count = info.array_elements > 5 ? 5 : info.array_elements;
        for (int i = 0; i < print_count; i++) {
            for (int j = 0; j < indent + 1; j++) printf("  ");
            printf("[%d] = ", i);
            /* For struct elements, recursively print the struct */
            type_info elem_info = dwarf_get_type_info(info.element_type.c_str());
            if (elem_info.is_struct) {
                printf("{\n");
                for (const auto& m : elem_info.members) {
                    print_member_value(pid, m.name.c_str(), addr + i * elem_size + m.offset, m.type_name, indent + 2);
                }
                for (int j = 0; j < indent + 1; j++) printf("  ");
                printf("},\n");
            } else if (elem_info.is_pointer) {
                /* For pointer elements, print pointer value */
                long val = ptrace(PTRACE_PEEKDATA, pid, addr + i * elem_size, nullptr);
                uint64_t ptr_val = static_cast<uint64_t>(val);
                if (is_char_pointer(info.element_type) && ptr_val != 0) {
                    std::string str = read_tracee_string(pid, ptr_val, 32);
                    printf("0x%lx \"%s\"\n", (unsigned long)ptr_val, str.c_str());
                } else {
                    printf("(%s) 0x%lx\n", info.element_type.c_str(), (unsigned long)ptr_val);
                }
            } else {
                /* For basic type elements, print value directly */
                long val = ptrace(PTRACE_PEEKDATA, pid, addr + i * elem_size, nullptr);
                if (elem_info.size == 1) val = (int8_t)val;
                else if (elem_info.size == 2) val = (int16_t)val;
                else if (elem_info.size == 4) val = (int32_t)val;
                printf("%ld\n", val);
            }
        }
        if (info.array_elements > 5) {
            for (int j = 0; j < indent + 1; j++) printf("  ");
            printf("... (%zu more elements)\n", info.array_elements - 5);
        }
        for (int i = 0; i < indent; i++) printf("  ");
        if (is_member) {
            printf("],\n");
        } else {
            printf("]\n");
        }
    } else if (info.is_pointer) {
        /* Print pointer */
        long val = ptrace(PTRACE_PEEKDATA, pid, addr, nullptr);
        uint64_t ptr_val = static_cast<uint64_t>(val);
        
        /* Special handling for char* - print as string */
        if (is_char_pointer(type_name) && ptr_val != 0 && !is_member) {
            std::string str = read_tracee_string(pid, ptr_val, 64);
            printf("%s = 0x%lx \"%s\"\n", expr, (unsigned long)ptr_val, str.c_str());
        } else if (is_member) {
            printf("%s = (%s) 0x%lx,\n", expr, type_name.c_str(), (unsigned long)ptr_val);
        } else {
            printf("%s = (%s) 0x%lx\n", expr, type_name.c_str(), (unsigned long)ptr_val);
        }
    } else {
        /* Basic type */
        long val = ptrace(PTRACE_PEEKDATA, pid, addr, nullptr);
        /* Truncate based on member size */
        if (info.size == 1) val = (int8_t)val;
        else if (info.size == 2) val = (int16_t)val;
        else if (info.size == 4) val = (int32_t)val;
        if (is_member) {
            printf("%s = %ld,\n", expr, val);
        } else {
            printf("%s = %ld\n", expr, val);
        }
    }
}

/* Print expression result in GDB style
 * For structs/arrays, prints detailed formatted output
 */
void expr_print(const char *e) {
    if (!e || !*e) return;
    
    init_parser_globals();
    
    int current_pid = get_current_pid();
    
    ExprNode* ast = parse_expression(e);
    if (!ast) {
        printf("Error parsing expression: %s\n", e);
        return;
    }
    
    /* Get target pid for process-qualified expressions (traceeN())
     * If not specified, use current pid */
    int target_pid = ast->get_target_pid();
    if (target_pid <= 0) {
        target_pid = current_pid;
    }
    
    /* Handle typeof specially - print the type name */
    if (is_typeof_expr(e)) {
        bool ok;
        EvalResult result = ast->eval(target_pid, ok);
        if (ok && !result.type_name.empty()) {
            printf("type = %s\n", result.type_name.c_str());
        } else {
            printf("type = unknown\n");
        }
        delete ast;
        return;
    }
    
    /* Try to get address and type for detailed printing */
    bool ok = true;
    EvalResult result = ast->eval_address(target_pid, ok);
    
    if (ok) {
        uint64_t addr = result.as_address();
        print_value_recursive(target_pid, e, addr, result.type_name, 0, false, true);
    } else {
        /* Just a value */
        result = ast->eval(target_pid, ok);
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
