#pragma once
#ifndef MYIR_H
#define MYIR_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdint>

namespace MyIR {

enum class PortDir { Input, Output, InOut, Internal };

struct Port {
    std::string name;
    PortDir     dir       = PortDir::Internal;
    int         width     = 1;
    bool        is_signed = false;
};

enum class OpType {
    And, Or, Xor, Not,
    Eq, Ne, Lt, Le, Gt, Ge,
    Add, Sub, Mul,
    Mux,
    Buf, Pos, Neg,
    Reduce_And, Reduce_Or, Reduce_Xor, Reduce_Xnor, Reduce_Bool,
    Shl, Shr, Sshl, Sshr,
    Logic_And, Logic_Or, Logic_Not,
    Unknown
};

struct Expr {
    OpType      op = OpType::Unknown;
    std::string result_wire;
    int         result_width = 1;
    std::vector<std::string> operands;
};

struct Assign {
    std::string lhs;
    std::string rhs;
};

struct Module {
    std::string                  name;
    std::vector<Port>            ports;
    std::map<std::string, Port>  wires;
    std::vector<Expr>            exprs;
    std::vector<Assign>          connects;
};

struct Design {
    std::map<std::string, Module> modules;
    void clear() { modules.clear(); }
};

Design* get_design();
void    set_design(Design *d);
void    ensure_design();

const char*  optype_to_string(OpType op);
OpType       string_to_optype(const std::string &s);
OpType       celltype_to_optype(const std::string &type);
std::string  optype_to_celltype(OpType op);
int          optype_num_inputs(OpType op);
bool         is_constant_name(const std::string &name);

uint64_t     const_to_uint(const std::string &name);
int          const_width(const std::string &name);
std::string  uint_to_const(uint64_t val, int width);

} // namespace MyIR

#endif
