/* expr_ast.cpp - AST node implementations */
#include "expr_ast.hpp"
#include "dwarf_info.h"
#include <cstring>
#include <sys/ptrace.h>
#include <cerrno>
#include <cstdio>

// namespace removed

/* Globals */
int g_num_processes = 0;
PtmcState g_ptmc_state = {nullptr};

/* Parser globals - defined here, declared in parser */
ExprNode* g_parser_result = nullptr;
const char* g_input_string = nullptr;

/* External parser interface */
extern int yyparse(void);
extern void yy_scan_string(const char* str);
extern void yylex_destroy(void);

/* Base class */
ExprNode::~ExprNode() = default;

EvalResult ExprNode::eval_address(int pid, bool& success) const {
    success = false;
    return EvalResult(0L);
}

/* Parser interface */
ExprNode* parse_expression(const char* input) {
    g_parser_result = nullptr;
    yy_scan_string(input);
    int result = yyparse();
    yylex_destroy();
    if (result != 0) {
        return nullptr;
    }
    return g_parser_result;
}

ExprNode* parse_expression(const std::string& input) {
    return parse_expression(input.c_str());
}

void free_parser_result() {
    delete g_parser_result;
    g_parser_result = nullptr;
}

/* Memory read */
static long read_memory_long(int pid, uint64_t addr, bool& success) {
    errno = 0;
    long data = ptrace(PTRACE_PEEKDATA, pid, addr, nullptr);
    if (errno != 0) {
        success = false;
        return 0;
    }
    success = true;
    return data;
}

static int get_type_size(const type_info& info) {
    if (info.is_pointer) return 8;
    if (info.size > 0) return info.size;
    return 4;
}

/* NumberNode */
EvalResult NumberNode::eval(int pid, bool& success) const {
    success = true;
    return EvalResult(value_);
}

/* TypeNode */
EvalResult TypeNode::eval(int pid, bool& success) const {
    success = false;
    return EvalResult(0L);
}

/* VariableNode */
EvalResult VariableNode::eval(int pid, bool& success) const {
    auto addr_res = eval_address(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = addr_res.as_address();
    std::string type_name = addr_res.type_name;
    
    if (!type_name.empty()) {
        type_info info = dwarf_get_type_info(type_name.c_str());
        
        if (info.is_struct || info.is_array) {
            EvalResult res(addr);
            res.type_name = type_name;
            return res;
        }
        
        int size = get_type_size(info);
        long val = read_memory_long(pid, addr, success);
        
        if (size == 1) val = static_cast<int8_t>(val);
        else if (size == 2) val = static_cast<int16_t>(val);
        else if (size == 4) val = static_cast<int32_t>(val);
        
        EvalResult res(val);
        res.type_name = type_name;
        return res;
    }
    
    long val = read_memory_long(pid, addr, success);
    return EvalResult(val);
}

EvalResult VariableNode::eval_address(int pid, bool& success) const {
    uintptr_t addr = dwarf_get_global_addr(name_.c_str());
    if (addr != 0) {
        success = true;
        EvalResult res(static_cast<uint64_t>(addr));
        res.type_name = dwarf_get_global_type(name_.c_str());
        return res;
    }
    
    std::string local_type;
    if (dwarf_lookup_local(name_.c_str(), &addr, &local_type)) {
        success = true;
        EvalResult res(static_cast<uint64_t>(addr));
        res.type_name = local_type;
        return res;
    }
    
    success = false;
    return EvalResult(0L);
}

/* BinaryOpNode */
EvalResult BinaryOpNode::eval(int pid, bool& success) const {
    EvalResult left = left_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    if (op_ == BinaryOp::AND) {
        if (!left.as_value()) return EvalResult(0L);
        EvalResult right = right_->eval(pid, success);
        if (!success) return EvalResult(0L);
        return EvalResult(right.as_value() ? 1L : 0L);
    }
    
    if (op_ == BinaryOp::OR) {
        if (left.as_value()) return EvalResult(1L);
        EvalResult right = right_->eval(pid, success);
        if (!success) return EvalResult(0L);
        return EvalResult(right.as_value() ? 1L : 0L);
    }
    
    EvalResult right = right_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    long l = left.as_value();
    long r = right.as_value();
    long result = 0;
    
    switch (op_) {
        case BinaryOp::ADD: result = l + r; break;
        case BinaryOp::SUB: result = l - r; break;
        case BinaryOp::MUL: result = l * r; break;
        case BinaryOp::DIV: result = r != 0 ? l / r : 0; break;
        case BinaryOp::MOD: result = r != 0 ? l % r : 0; break;
        case BinaryOp::LT:  result = l < r ? 1 : 0; break;
        case BinaryOp::GT:  result = l > r ? 1 : 0; break;
        case BinaryOp::LE:  result = l <= r ? 1 : 0; break;
        case BinaryOp::GE:  result = l >= r ? 1 : 0; break;
        case BinaryOp::EQ:  result = l == r ? 1 : 0; break;
        case BinaryOp::NE:  result = l != r ? 1 : 0; break;
        case BinaryOp::BITAND: result = l & r; break;
        case BinaryOp::BITOR:  result = l | r; break;
        case BinaryOp::XOR:    result = l ^ r; break;
        case BinaryOp::SHL:    result = l << r; break;
        case BinaryOp::SHR:    result = l >> r; break;
    }
    
    success = true;
    return EvalResult(result);
}

std::string BinaryOpNode::to_string() const {
    const char* op_str = "";
    switch (op_) {
        case BinaryOp::ADD: op_str = "+"; break;
        case BinaryOp::SUB: op_str = "-"; break;
        case BinaryOp::MUL: op_str = "*"; break;
        case BinaryOp::DIV: op_str = "/"; break;
        case BinaryOp::MOD: op_str = "%"; break;
        case BinaryOp::LT:  op_str = "<"; break;
        case BinaryOp::GT:  op_str = ">"; break;
        case BinaryOp::LE:  op_str = "<="; break;
        case BinaryOp::GE:  op_str = ">="; break;
        case BinaryOp::EQ:  op_str = "=="; break;
        case BinaryOp::NE:  op_str = "!="; break;
        case BinaryOp::AND: op_str = "&&"; break;
        case BinaryOp::OR:  op_str = "||"; break;
        case BinaryOp::BITAND: op_str = "&"; break;
        case BinaryOp::BITOR:  op_str = "|"; break;
        case BinaryOp::XOR:    op_str = "^"; break;
        case BinaryOp::SHL:    op_str = "<<"; break;
        case BinaryOp::SHR:    op_str = ">>"; break;
    }
    return "(" + left_->to_string() + " " + op_str + " " + right_->to_string() + ")";
}

/* UnaryOpNode */
EvalResult UnaryOpNode::eval(int pid, bool& success) const {
    EvalResult op = operand_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    long v = op.as_value();
    long result = 0;
    
    switch (op_) {
        case UnaryOp::NEG:    result = -v; break;
        case UnaryOp::NOT:    result = !v ? 1 : 0; break;
        case UnaryOp::BITNOT: result = ~v; break;
    }
    
    success = true;
    return EvalResult(result);
}

std::string UnaryOpNode::to_string() const {
    const char* op_str = "";
    switch (op_) {
        case UnaryOp::NEG:    op_str = "-"; break;
        case UnaryOp::NOT:    op_str = "!"; break;
        case UnaryOp::BITNOT: op_str = "~"; break;
    }
    return std::string(op_str) + operand_->to_string();
}

/* PrefixOpNode */
EvalResult PrefixOpNode::eval(int pid, bool& success) const {
    EvalResult addr_res = operand_->eval_address(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = addr_res.as_address();
    long old_val = read_memory_long(pid, addr, success);
    if (!success) return EvalResult(0L);
    
    long new_val = (op_ == PrefixOp::PRE_INC) ? old_val + 1 : old_val - 1;
    
    if (ptrace(PTRACE_POKEDATA, pid, addr, new_val) < 0) {
        success = false;
        return EvalResult(0L);
    }
    
    success = true;
    return EvalResult(new_val);
}

std::string PrefixOpNode::to_string() const {
    return std::string(op_ == PrefixOp::PRE_INC ? "++" : "--") + operand_->to_string();
}

/* PostfixOpNode */
EvalResult PostfixOpNode::eval(int pid, bool& success) const {
    EvalResult addr_res = operand_->eval_address(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = addr_res.as_address();
    long old_val = read_memory_long(pid, addr, success);
    if (!success) return EvalResult(0L);
    
    long new_val = (op_ == PostfixOp::POST_INC) ? old_val + 1 : old_val - 1;
    
    if (ptrace(PTRACE_POKEDATA, pid, addr, new_val) < 0) {
        success = false;
        return EvalResult(0L);
    }
    
    success = true;
    return EvalResult(old_val);
}

std::string PostfixOpNode::to_string() const {
    return operand_->to_string() + (op_ == PostfixOp::POST_INC ? "++" : "--");
}

/* MemberAccessNode */
EvalResult MemberAccessNode::eval(int pid, bool& success) const {
    /* First get the base object type (parent type) */
    std::string parent_type;
    bool dummy;
    EvalResult obj_res = object_->eval(pid, dummy);
    if (!obj_res.type_name.empty()) {
        parent_type = obj_res.type_name;
        /* For pointer, get the pointed-to type */
        type_info info = dwarf_get_type_info(parent_type.c_str());
        if (info.is_pointer && !info.element_type.empty()) {
            parent_type = info.element_type;
        }
    }
    
    EvalResult addr_res = eval_address(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = addr_res.as_address();
    std::string member_type = addr_res.type_name;
    
    /* Use parent_type to find member size for correct truncation */
    if (!parent_type.empty()) {
        type_info info = dwarf_get_type_info(parent_type.c_str());
        
        for (const auto& m : info.members) {
            if (m.name == member_) {
                type_info member_info = dwarf_get_type_info(m.type_name.c_str());
                if (member_info.is_struct || member_info.is_array) {
                    EvalResult res(addr);
                    res.type_name = m.type_name;
                    return res;
                }
                
                long val = read_memory_long(pid, addr, success);
                /* Truncate based on actual member size */
                if (m.size == 1) val = static_cast<int8_t>(val);
                else if (m.size == 2) val = static_cast<int16_t>(val);
                else if (m.size == 4) val = static_cast<int32_t>(val);
                
                EvalResult res(val);
                res.type_name = m.type_name;
                return res;
            }
        }
    }
    
    /* Fallback: just read value */
    long val = read_memory_long(pid, addr, success);
    return EvalResult(val);
}

EvalResult MemberAccessNode::eval_address(int pid, bool& success) const {
    if (!object_) {
        success = false;
        return EvalResult(0L);
    }
    
    EvalResult base_res;
    std::string base_type;
    
    if (op_ == MemberOp::ARROW) {
        base_res = object_->eval(pid, success);
        if (!success) return EvalResult(0L);
        base_type = base_res.type_name;
        
        if (!base_type.empty()) {
            type_info info = dwarf_get_type_info(base_type.c_str());
            if (info.is_pointer && !info.element_type.empty()) {
                base_type = info.element_type;
            }
        }
    } else {
        base_res = object_->eval_address(pid, success);
        if (!success) return EvalResult(0L);
        base_type = base_res.type_name;
    }
    
    uint64_t base_addr = base_res.is_address ? base_res.as_address() : 
                                               static_cast<uint64_t>(base_res.as_value());
    
    if (base_type.empty()) {
        success = false;
        return EvalResult(0L);
    }
    
    ptrdiff_t offset = dwarf_member_offset(base_type.c_str(), member_.c_str());
    if (offset < 0) {
        success = false;
        return EvalResult(0L);
    }
    
    success = true;
    EvalResult res(base_addr + offset);
    
    std::string member_type;
    size_t member_size;
    if (dwarf_member_info(base_type.c_str(), member_.c_str(), &offset, &member_size, &member_type)) {
        res.type_name = member_type;
    }
    
    return res;
}

std::string MemberAccessNode::to_string() const {
    return object_->to_string() + (op_ == MemberOp::ARROW ? "->" : ".") + member_;
}

/* ArrayIndexNode */
EvalResult ArrayIndexNode::eval(int pid, bool& success) const {
    EvalResult addr_res = eval_address(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = addr_res.as_address();
    std::string elem_type = addr_res.type_name;
    
    if (!elem_type.empty()) {
        type_info info = dwarf_get_type_info(elem_type.c_str());
        
        if (info.is_struct || info.is_array) {
            EvalResult res(addr);
            res.type_name = elem_type;
            return res;
        }
        
        long val = read_memory_long(pid, addr, success);
        int size = get_type_size(info);
        if (size == 1) val = static_cast<int8_t>(val);
        else if (size == 2) val = static_cast<int16_t>(val);
        else if (size == 4) val = static_cast<int32_t>(val);
        
        EvalResult res(val);
        res.type_name = elem_type;
        return res;
    }
    
    long val = read_memory_long(pid, addr, success);
    return EvalResult(val);
}

EvalResult ArrayIndexNode::eval_address(int pid, bool& success) const {
    EvalResult base_res = array_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t base_addr = base_res.is_address ? base_res.as_address() : 
                                               static_cast<uint64_t>(base_res.as_value());
    
    EvalResult idx_res = index_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    long index = idx_res.as_value();
    
    int elem_size = 8;
    std::string elem_type;
    
    if (!base_res.type_name.empty()) {
        type_info info = dwarf_get_type_info(base_res.type_name.c_str());
        if (info.is_array && !info.element_type.empty()) {
            elem_type = info.element_type;
            type_info elem_info = dwarf_get_type_info(elem_type.c_str());
            elem_size = get_type_size(elem_info);
        }
    }
    
    success = true;
    EvalResult res(base_addr + index * elem_size);
    res.type_name = elem_type;
    return res;
}

std::string ArrayIndexNode::to_string() const {
    return array_->to_string() + "[" + index_->to_string() + "]";
}

/* AddressOfNode */
EvalResult AddressOfNode::eval(int pid, bool& success) const {
    return operand_->eval_address(pid, success);
}

std::string AddressOfNode::to_string() const {
    return "&" + operand_->to_string();
}

/* DereferenceNode */
EvalResult DereferenceNode::eval(int pid, bool& success) const {
    EvalResult ptr_res = operand_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = ptr_res.is_address ? ptr_res.as_address() : 
                                         static_cast<uint64_t>(ptr_res.as_value());
    
    std::string inner_type;
    if (!ptr_res.type_name.empty()) {
        type_info info = dwarf_get_type_info(ptr_res.type_name.c_str());
        if (info.is_pointer && !info.element_type.empty()) {
            inner_type = info.element_type;
        }
    }
    
    if (!inner_type.empty()) {
        type_info info = dwarf_get_type_info(inner_type.c_str());
        
        if (info.is_struct || info.is_array) {
            EvalResult res(addr);
            res.type_name = inner_type;
            return res;
        }
        
        long val = read_memory_long(pid, addr, success);
        int size = get_type_size(info);
        if (size == 1) val = static_cast<int8_t>(val);
        else if (size == 2) val = static_cast<int16_t>(val);
        else if (size == 4) val = static_cast<int32_t>(val);
        
        EvalResult res(val);
        res.type_name = inner_type;
        return res;
    }
    
    long val = read_memory_long(pid, addr, success);
    return EvalResult(val);
}

EvalResult DereferenceNode::eval_address(int pid, bool& success) const {
    EvalResult ptr_res = operand_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = ptr_res.is_address ? ptr_res.as_address() : 
                                         static_cast<uint64_t>(ptr_res.as_value());
    
    std::string inner_type;
    if (!ptr_res.type_name.empty()) {
        type_info info = dwarf_get_type_info(ptr_res.type_name.c_str());
        if (info.is_pointer && !info.element_type.empty()) {
            inner_type = info.element_type;
        }
    }
    
    success = true;
    EvalResult res(addr);
    res.type_name = inner_type;
    return res;
}

std::string DereferenceNode::to_string() const {
    return "*" + operand_->to_string();
}

/* ConditionalNode */
EvalResult ConditionalNode::eval(int pid, bool& success) const {
    EvalResult c = cond_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    if (c.as_value()) {
        return then_->eval(pid, success);
    } else {
        return else_->eval(pid, success);
    }
}

std::string ConditionalNode::to_string() const {
    return cond_->to_string() + " ? " + then_->to_string() + " : " + else_->to_string();
}

/* AssignNode */
EvalResult AssignNode::eval(int pid, bool& success) const {
    EvalResult addr_res = left_->eval_address(pid, success);
    if (!success) return EvalResult(0L);
    
    uint64_t addr = addr_res.as_address();
    
    EvalResult val_res = right_->eval(pid, success);
    if (!success) return EvalResult(0L);
    
    long val = val_res.as_value();
    
    if (ptrace(PTRACE_POKEDATA, pid, addr, val) < 0) {
        success = false;
        return EvalResult(0L);
    }
    
    success = true;
    return EvalResult(val);
}

std::string AssignNode::to_string() const {
    return left_->to_string() + " = " + right_->to_string();
}

/* CastNode */
EvalResult CastNode::eval(int pid, bool& success) const {
    return operand_->eval(pid, success);
}

std::string CastNode::to_string() const {
    return "(" + type_->to_string() + ")" + operand_->to_string();
}

/* SizeofExprNode */
EvalResult SizeofExprNode::eval(int pid, bool& success) const {
    bool dummy;
    EvalResult op_res = operand_->eval(pid, dummy);
    
    int size = 8;
    if (!op_res.type_name.empty()) {
        size = dwarf_type_size(op_res.type_name.c_str());
    }
    
    success = true;
    return EvalResult(static_cast<long>(size));
}

std::string SizeofExprNode::to_string() const {
    return "sizeof(" + operand_->to_string() + ")";
}

/* SizeofTypeNode */
EvalResult SizeofTypeNode::eval(int pid, bool& success) const {
    int size = dwarf_type_size(type_name_.c_str());
    success = true;
    return EvalResult(static_cast<long>(size));
}

std::string SizeofTypeNode::to_string() const {
    return "sizeof(" + type_name_ + ")";
}

/* OffsetofNode */
EvalResult OffsetofNode::eval(int pid, bool& success) const {
    ptrdiff_t offset = dwarf_member_offset(type_name_.c_str(), member_.c_str());
    if (offset < 0) {
        success = false;
        return EvalResult(0L);
    }
    success = true;
    return EvalResult(static_cast<long>(offset));
}

std::string OffsetofNode::to_string() const {
    return "offsetof(" + type_name_ + ", " + member_ + ")";
}

/* ProcessQualifiedNode */
int ProcessQualifiedNode::get_target_pid() const {
    if (proc_id_ >= 0 && proc_id_ < g_num_processes && g_ptmc_state.pids) {
        return g_ptmc_state.pids[proc_id_];
    }
    return 0;
}

EvalResult ProcessQualifiedNode::eval(int pid, bool& success) const {
    int target_pid = get_target_pid();
    if (target_pid == 0) {
        success = false;
        return EvalResult(0L);
    }
    return expr_->eval(target_pid, success);
}

EvalResult ProcessQualifiedNode::eval_address(int pid, bool& success) const {
    int target_pid = get_target_pid();
    if (target_pid == 0) {
        success = false;
        return EvalResult(0L);
    }
    return expr_->eval_address(target_pid, success);
}

std::string ProcessQualifiedNode::to_string() const {
    return "tracee" + std::to_string(proc_id_) + "(" + expr_->to_string() + ")";
}

// namespace end
