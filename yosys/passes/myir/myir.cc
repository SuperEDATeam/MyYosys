#include "passes/myir/myir.h"
#include <cctype>

namespace MyIR {

static Design *g_myir_design = nullptr;

Design* get_design()            { return g_myir_design; }
void    set_design(Design *d)   { g_myir_design = d; }

void ensure_design() {
    if (!g_myir_design)
        g_myir_design = new Design();
}

// ----------------------------------------------------------------
//  OpType <-> human-readable string
// ----------------------------------------------------------------

const char* optype_to_string(OpType op) {
    switch (op) {
    case OpType::And:          return "And";
    case OpType::Or:           return "Or";
    case OpType::Xor:          return "Xor";
    case OpType::Xnor:         return "Xnor";
    case OpType::Not:          return "Not";
    case OpType::Eq:           return "Eq";
    case OpType::Ne:           return "Ne";
    case OpType::Lt:           return "Lt";
    case OpType::Le:           return "Le";
    case OpType::Gt:           return "Gt";
    case OpType::Ge:           return "Ge";
    case OpType::Add:          return "Add";
    case OpType::Sub:          return "Sub";
    case OpType::Mul:          return "Mul";
    case OpType::Mux:          return "Mux";
    case OpType::Concat:       return "Concat";
    case OpType::Buf:          return "Buf";
    case OpType::Pos:          return "Pos";
    case OpType::Neg:          return "Neg";
    case OpType::Reduce_And:   return "ReduceAnd";
    case OpType::Reduce_Or:    return "ReduceOr";
    case OpType::Reduce_Xor:   return "ReduceXor";
    case OpType::Reduce_Xnor:  return "ReduceXnor";
    case OpType::Reduce_Bool:  return "ReduceBool";
    case OpType::Shl:          return "Shl";
    case OpType::Shr:          return "Shr";
    case OpType::Sshl:         return "Sshl";
    case OpType::Sshr:         return "Sshr";
    case OpType::Logic_And:    return "LogicAnd";
    case OpType::Logic_Or:     return "LogicOr";
    case OpType::Logic_Not:    return "LogicNot";
    default:                   return "Unknown";
    }
}

OpType string_to_optype(const std::string &s) {
    if (s == "And")         return OpType::And;
    if (s == "Or")          return OpType::Or;
    if (s == "Xor")         return OpType::Xor;
    if (s == "Xnor")        return OpType::Xnor;
    if (s == "Not")         return OpType::Not;
    if (s == "Eq")          return OpType::Eq;
    if (s == "Ne")          return OpType::Ne;
    if (s == "Lt")          return OpType::Lt;
    if (s == "Le")          return OpType::Le;
    if (s == "Gt")          return OpType::Gt;
    if (s == "Ge")          return OpType::Ge;
    if (s == "Add")         return OpType::Add;
    if (s == "Sub")         return OpType::Sub;
    if (s == "Mul")         return OpType::Mul;
    if (s == "Mux")         return OpType::Mux;
    if (s == "Concat")      return OpType::Concat;
    if (s == "Buf")         return OpType::Buf;
    if (s == "Pos")         return OpType::Pos;
    if (s == "Neg")         return OpType::Neg;
    if (s == "ReduceAnd")   return OpType::Reduce_And;
    if (s == "ReduceOr")    return OpType::Reduce_Or;
    if (s == "ReduceXor")   return OpType::Reduce_Xor;
    if (s == "ReduceXnor")  return OpType::Reduce_Xnor;
    if (s == "ReduceBool")  return OpType::Reduce_Bool;
    if (s == "Shl")         return OpType::Shl;
    if (s == "Shr")         return OpType::Shr;
    if (s == "Sshl")        return OpType::Sshl;
    if (s == "Sshr")        return OpType::Sshr;
    if (s == "LogicAnd")    return OpType::Logic_And;
    if (s == "LogicOr")     return OpType::Logic_Or;
    if (s == "LogicNot")    return OpType::Logic_Not;
    return OpType::Unknown;
}

// ----------------------------------------------------------------
//  RTLIL cell type string <-> OpType
// ----------------------------------------------------------------

OpType celltype_to_optype(const std::string &type) {
    if (type == "$and")          return OpType::And;
    if (type == "$or")           return OpType::Or;
    if (type == "$xor")          return OpType::Xor;
    if (type == "$xnor")         return OpType::Xnor;
    if (type == "$not")          return OpType::Not;
    if (type == "$eq")           return OpType::Eq;
    if (type == "$ne")           return OpType::Ne;
    if (type == "$lt")           return OpType::Lt;
    if (type == "$le")           return OpType::Le;
    if (type == "$gt")           return OpType::Gt;
    if (type == "$ge")           return OpType::Ge;
    if (type == "$add")          return OpType::Add;
    if (type == "$sub")          return OpType::Sub;
    if (type == "$mul")          return OpType::Mul;
    if (type == "$mux")          return OpType::Mux;
    if (type == "$buf")          return OpType::Buf;
    if (type == "$pos")          return OpType::Pos;
    if (type == "$neg")          return OpType::Neg;
    if (type == "$reduce_and")   return OpType::Reduce_And;
    if (type == "$reduce_or")    return OpType::Reduce_Or;
    if (type == "$reduce_xor")   return OpType::Reduce_Xor;
    if (type == "$reduce_xnor")  return OpType::Reduce_Xnor;
    if (type == "$reduce_bool")  return OpType::Reduce_Bool;
    if (type == "$shl")          return OpType::Shl;
    if (type == "$shr")          return OpType::Shr;
    if (type == "$sshl")         return OpType::Sshl;
    if (type == "$sshr")         return OpType::Sshr;
    if (type == "$logic_and")    return OpType::Logic_And;
    if (type == "$logic_or")     return OpType::Logic_Or;
    if (type == "$logic_not")    return OpType::Logic_Not;
    return OpType::Unknown;
}

std::string optype_to_celltype(OpType op) {
    switch (op) {
    case OpType::And:          return "$and";
    case OpType::Or:           return "$or";
    case OpType::Xor:          return "$xor";
    case OpType::Xnor:         return "$xnor";
    case OpType::Not:          return "$not";
    case OpType::Eq:           return "$eq";
    case OpType::Ne:           return "$ne";
    case OpType::Lt:           return "$lt";
    case OpType::Le:           return "$le";
    case OpType::Gt:           return "$gt";
    case OpType::Ge:           return "$ge";
    case OpType::Add:          return "$add";
    case OpType::Sub:          return "$sub";
    case OpType::Mul:          return "$mul";
    case OpType::Mux:          return "$mux";
    case OpType::Concat:       return "$concat";
    case OpType::Buf:          return "$buf";
    case OpType::Pos:          return "$pos";
    case OpType::Neg:          return "$neg";
    case OpType::Reduce_And:   return "$reduce_and";
    case OpType::Reduce_Or:    return "$reduce_or";
    case OpType::Reduce_Xor:   return "$reduce_xor";
    case OpType::Reduce_Xnor:  return "$reduce_xnor";
    case OpType::Reduce_Bool:  return "$reduce_bool";
    case OpType::Shl:          return "$shl";
    case OpType::Shr:          return "$shr";
    case OpType::Sshl:         return "$sshl";
    case OpType::Sshr:         return "$sshr";
    case OpType::Logic_And:    return "$logic_and";
    case OpType::Logic_Or:     return "$logic_or";
    case OpType::Logic_Not:    return "$logic_not";
    default:                   return "";
    }
}

// ----------------------------------------------------------------
//  Operand count per OpType
// ----------------------------------------------------------------

int optype_num_inputs(OpType op) {
    switch (op) {
    case OpType::Not:
    case OpType::Buf:
    case OpType::Pos:
    case OpType::Neg:
    case OpType::Reduce_And:
    case OpType::Reduce_Or:
    case OpType::Reduce_Xor:
    case OpType::Reduce_Xnor:
    case OpType::Reduce_Bool:
    case OpType::Logic_Not:
        return 1;
    case OpType::Mux:
        return 3;   // A, B, S
    case OpType::Concat:
        return -1;  // variable number of operands
    default:
        return 2;   // A, B
    }
}

// ----------------------------------------------------------------
//  Constant literal helpers
// ----------------------------------------------------------------

bool is_constant_name(const std::string &name) {
    if (name.empty()) return false;
    size_t pos = name.find('\'');
    if (pos == std::string::npos || pos == 0) return false;
    for (size_t i = 0; i < pos; i++)
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    return true;
}

uint64_t const_to_uint(const std::string &name) {
    size_t pos = name.find('\'');
    if (pos == std::string::npos) return 0;
    std::string bits_str = name.substr(pos + 1);
    uint64_t val = 0;
    for (char c : bits_str)
        val = (val << 1) | (c == '1' ? 1ULL : 0ULL);
    return val;
}

int const_width(const std::string &name) {
    size_t pos = name.find('\'');
    if (pos == std::string::npos) return 0;
    return std::stoi(name.substr(0, pos));
}

std::string uint_to_const(uint64_t val, int width) {
    if (width <= 0) width = 1;
    std::string bits;
    for (int i = width - 1; i >= 0; i--)
        bits += ((val >> i) & 1) ? '1' : '0';
    return std::to_string(width) + "'" + bits;
}

const char* stmt_kind_str(Statement::Kind k) {
    switch (k) {
    case Statement::Assign: return "assign";
    case Statement::If:     return "if";
    case Statement::Case:   return "case";
    case Statement::Block:  return "block";
    default:                return "?";
    }
}

const char* sens_type_str(SensItem::Type t) {
    switch (t) {
    case SensItem::Star:    return "*";
    case SensItem::Posedge: return "posedge";
    case SensItem::Negedge: return "negedge";
    case SensItem::Level:   return "level";
    default:                return "?";
    }
}

} // namespace MyIR
