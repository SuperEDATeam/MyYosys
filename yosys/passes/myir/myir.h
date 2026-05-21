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
    And, Or, Xor, Xnor, Not,
    Eq, Ne, Lt, Le, Gt, Ge,
    Add, Sub, Mul,
    Mux, Concat,
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

// ----- Procedural block types (from AST) -----

struct ProcAssign {
    bool        is_blocking = true;
    std::string lhs;
    std::string rhs;
};

struct Statement;

struct IfElse {
    std::string condition;
    std::vector<Statement> then_body;
    std::vector<Statement> else_body;
};

struct CaseItem {
    std::vector<std::string> values;   // empty = default
    std::vector<Statement>   body;
};

struct CaseStmt {
    std::string            select;
    std::vector<CaseItem>  items;
};

struct Statement {
    enum Kind { Assign, If, Case, Block } kind = Assign;
    ProcAssign             proc_assign;
    IfElse                 if_else;
    CaseStmt               case_stmt;
    std::vector<Statement> block_body;
};

struct SensItem {
    enum Type { Star, Posedge, Negedge, Level } type = Star;
    std::string signal;
};

struct ProcBlock {
    std::vector<SensItem>  sensitivity;
    std::vector<Statement> body;
};

// ----- Module / Design -----

struct Module {
    std::string                  name;
    std::vector<Port>            ports;
    std::map<std::string, Port>  wires;
    std::vector<Expr>            exprs;
    std::vector<Assign>          connects;
    std::vector<ProcBlock>       proc_blocks;
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

// Empirical benefit score for an IR transformation.
//
// Higher score = transformation is more profitable.  Used by the
// myir_opt_expr / myir_opt_muxtree / myir_rewrite passes to pick
// between competing rewrite candidates and to optionally guard
// non-profitable rewrites (score <= 0).
//
// Coefficients (current values, tunable in myir.cc):
//   removed_exprs    : +10  -- each removed Expr saves an RTLIL cell
//   removed_muxes    :  +6  -- muxes are slightly cheaper to remove
//                              than generic gates; weight kept below
//                              removed_exprs to bias toward larger
//                              rewrites
//   added_exprs      :  -3  -- each newly introduced Expr costs less
//                              than the cell it (typically) makes
//                              cheaper downstream
//   added_wires      :  -1  -- a fresh internal wire has marginal cost
//   added_connects   :  -1  -- a new direct connect is roughly equal
//                              to a wire
int          transform_benefit_score(int removed_exprs, int removed_muxes,
                                     int added_exprs, int added_wires,
                                     int added_connects = 0);

const char*  stmt_kind_str(Statement::Kind k);
const char*  sens_type_str(SensItem::Type t);

// ----- Visualization helpers (used by myir_show / myir_view) -----

// Directory containing the running yosys executable (trailing separator
// stripped).  Empty string on failure.
std::string get_executable_dir();

// Locate a usable Graphviz 'dot' executable.  Lookup order:
//   1. <exe_dir>/tools/graphviz/bin/dot(.exe)
//   2. <exe_dir>/../tools/graphviz/bin/dot(.exe)   (VS Debug/Release subdir)
//   3. Environment variable MYIR_DOT
//   4. System PATH
// Returns a shell-ready string (already quoted when it contains a full path)
// or an empty string if no 'dot' was found.
std::string find_dot_executable();

// Build an OS-specific "open with default associated application" command
// for the given file.  The returned string can be passed to run_command().
//   Windows : cmd /c start "" "<file>"   (non-blocking)
//   macOS   : open "<file>"
//   Linux   : xdg-open "<file>" >/dev/null 2>&1 &
std::string make_open_cmd(const std::string &file);

} // namespace MyIR

#endif
