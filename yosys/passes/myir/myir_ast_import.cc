#include "kernel/yosys.h"
#include "frontends/ast/ast.h"
#include "passes/myir/myir.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

using AST::AstNode;

// ================================================================
//  AST node type -> MyIR::OpType
// ================================================================

static MyIR::OpType ast_op(AST::AstNodeType t)
{
    switch (t) {
    case AST::AST_BIT_AND:      return MyIR::OpType::And;
    case AST::AST_BIT_OR:       return MyIR::OpType::Or;
    case AST::AST_BIT_XOR:      return MyIR::OpType::Xor;
    case AST::AST_BIT_XNOR:     return MyIR::OpType::Xnor;
    case AST::AST_BIT_NOT:      return MyIR::OpType::Not;
    case AST::AST_REDUCE_AND:   return MyIR::OpType::Reduce_And;
    case AST::AST_REDUCE_OR:    return MyIR::OpType::Reduce_Or;
    case AST::AST_REDUCE_XOR:   return MyIR::OpType::Reduce_Xor;
    case AST::AST_REDUCE_XNOR:  return MyIR::OpType::Reduce_Xnor;
    case AST::AST_REDUCE_BOOL:  return MyIR::OpType::Reduce_Bool;
    case AST::AST_SHIFT_LEFT:   return MyIR::OpType::Shl;
    case AST::AST_SHIFT_RIGHT:  return MyIR::OpType::Shr;
    case AST::AST_SHIFT_SLEFT:  return MyIR::OpType::Sshl;
    case AST::AST_SHIFT_SRIGHT: return MyIR::OpType::Sshr;
    case AST::AST_LT:           return MyIR::OpType::Lt;
    case AST::AST_LE:           return MyIR::OpType::Le;
    case AST::AST_EQ:           return MyIR::OpType::Eq;
    case AST::AST_NE:           return MyIR::OpType::Ne;
    case AST::AST_GE:           return MyIR::OpType::Ge;
    case AST::AST_GT:           return MyIR::OpType::Gt;
    case AST::AST_ADD:          return MyIR::OpType::Add;
    case AST::AST_SUB:          return MyIR::OpType::Sub;
    case AST::AST_MUL:          return MyIR::OpType::Mul;
    case AST::AST_POS:          return MyIR::OpType::Pos;
    case AST::AST_NEG:          return MyIR::OpType::Neg;
    case AST::AST_LOGIC_AND:    return MyIR::OpType::Logic_And;
    case AST::AST_LOGIC_OR:     return MyIR::OpType::Logic_Or;
    case AST::AST_LOGIC_NOT:    return MyIR::OpType::Logic_Not;
    case AST::AST_TERNARY:      return MyIR::OpType::Mux;
    default:                    return MyIR::OpType::Unknown;
    }
}

// ================================================================
//  Helpers: extract width from AST_WIRE / AST_CONSTANT
// ================================================================

static int const_bits_to_int(const std::vector<RTLIL::State> &bits)
{
    int val = 0;
    for (int i = (int)bits.size() - 1; i >= 0; i--)
        val = (val << 1) | (bits[i] == RTLIL::State::S1 ? 1 : 0);
    return val;
}

static int extract_range_width(AstNode *wire_node)
{
    if (wire_node->range_valid)
        return std::abs(wire_node->range_left - wire_node->range_right) + 1;

    for (auto *child : wire_node->children) {
        if (child->type == AST::AST_RANGE && child->children.size() >= 2) {
            auto *msb = child->children[0];
            auto *lsb = child->children[1];
            if (msb->type == AST::AST_CONSTANT && lsb->type == AST::AST_CONSTANT) {
                int left  = const_bits_to_int(msb->bits);
                int right = const_bits_to_int(lsb->bits);
                return std::abs(left - right) + 1;
            }
        }
        if (child->type == AST::AST_RANGE && child->children.size() == 1) {
            return 1; // single-bit select
        }
    }
    return 1;
}

static std::string bits_to_const_name(const std::vector<RTLIL::State> &bits)
{
    int w = (int)bits.size();
    if (w == 0) return "1'0";
    std::string s;
    for (int i = w - 1; i >= 0; i--)
        s += (bits[i] == RTLIL::State::S1) ? '1' : '0';
    return std::to_string(w) + "'" + s;
}

// ================================================================
//  Expression tree flattener
//  Returns the wire name holding the result (may be an original wire,
//  a constant literal, or a generated temporary).
// ================================================================

static std::string flatten_expr(AstNode *node, MyIR::Module &m, int &tmp_cnt)
{
    if (!node) return "1'0";

    switch (node->type)
    {
    case AST::AST_IDENTIFIER:
    {
        std::string name = node->str;
        for (auto *child : node->children) {
            if (child->type != AST::AST_RANGE) continue;
            if (child->children.size() == 2 &&
                child->children[0]->type == AST::AST_CONSTANT &&
                child->children[1]->type == AST::AST_CONSTANT) {
                int msb = const_bits_to_int(child->children[0]->bits);
                int lsb = const_bits_to_int(child->children[1]->bits);
                name += "[" + std::to_string(msb) + ":" + std::to_string(lsb) + "]";
                int w = std::abs(msb - lsb) + 1;
                if (m.wires.find(name) == m.wires.end())
                    m.wires[name] = MyIR::Port{name, MyIR::PortDir::Internal, w, false};
            } else if (child->children.size() == 1 &&
                       child->children[0]->type == AST::AST_CONSTANT) {
                int bit = const_bits_to_int(child->children[0]->bits);
                name += "[" + std::to_string(bit) + "]";
                if (m.wires.find(name) == m.wires.end())
                    m.wires[name] = MyIR::Port{name, MyIR::PortDir::Internal, 1, false};
            }
        }
        return name;
    }

    case AST::AST_CONSTANT:
        return bits_to_const_name(node->bits);

    case AST::AST_TERNARY:
    {
        if (node->children.size() < 3) break;
        std::string s = flatten_expr(node->children[0], m, tmp_cnt);
        std::string a = flatten_expr(node->children[1], m, tmp_cnt);
        std::string b = flatten_expr(node->children[2], m, tmp_cnt);
        std::string tmp = "$ast_t" + std::to_string(tmp_cnt++);

        int w = 1;
        auto wa = m.wires.find(a);
        if (wa != m.wires.end()) w = wa->second.width;

        m.wires[tmp] = MyIR::Port{tmp, MyIR::PortDir::Internal, w, false};
        m.exprs.push_back(MyIR::Expr{MyIR::OpType::Mux, tmp, w, {a, b, s}});
        return tmp;
    }

    default:
        break;
    }

    // Binary / unary operators
    MyIR::OpType op = ast_op(node->type);
    if (op != MyIR::OpType::Unknown)
    {
        int nargs = MyIR::optype_num_inputs(op);

        if (nargs == 1 && node->children.size() >= 1) {
            std::string a = flatten_expr(node->children[0], m, tmp_cnt);
            std::string tmp = "$ast_t" + std::to_string(tmp_cnt++);

            int w = 1;
            auto wa = m.wires.find(a);
            if (wa != m.wires.end()) w = wa->second.width;
            // Reductions always produce 1-bit result
            if (op == MyIR::OpType::Reduce_And || op == MyIR::OpType::Reduce_Or ||
                op == MyIR::OpType::Reduce_Xor || op == MyIR::OpType::Reduce_Xnor ||
                op == MyIR::OpType::Reduce_Bool || op == MyIR::OpType::Logic_Not)
                w = 1;

            m.wires[tmp] = MyIR::Port{tmp, MyIR::PortDir::Internal, w, false};
            m.exprs.push_back(MyIR::Expr{op, tmp, w, {a}});
            return tmp;
        }

        if (nargs >= 2 && node->children.size() >= 2) {
            std::string a = flatten_expr(node->children[0], m, tmp_cnt);
            std::string b = flatten_expr(node->children[1], m, tmp_cnt);
            std::string tmp = "$ast_t" + std::to_string(tmp_cnt++);

            int w = 1;
            auto wa = m.wires.find(a);
            auto wb = m.wires.find(b);
            if (wa != m.wires.end()) w = wa->second.width;
            if (wb != m.wires.end() && wb->second.width > w) w = wb->second.width;
            // Comparisons and logic ops always produce 1-bit result
            if (op == MyIR::OpType::Eq  || op == MyIR::OpType::Ne ||
                op == MyIR::OpType::Lt  || op == MyIR::OpType::Le ||
                op == MyIR::OpType::Gt  || op == MyIR::OpType::Ge ||
                op == MyIR::OpType::Logic_And || op == MyIR::OpType::Logic_Or)
                w = 1;

            m.wires[tmp] = MyIR::Port{tmp, MyIR::PortDir::Internal, w, false};
            m.exprs.push_back(MyIR::Expr{op, tmp, w, {a, b}});
            return tmp;
        }
    }

    if (node->type == AST::AST_CONCAT) {
        std::vector<std::string> parts;
        int total_width = 0;
        for (auto *ch : node->children) {
            std::string part = flatten_expr(ch, m, tmp_cnt);
            parts.push_back(part);
            auto wit = m.wires.find(part);
            if (wit != m.wires.end())
                total_width += wit->second.width;
            else if (MyIR::is_constant_name(part))
                total_width += MyIR::const_width(part);
            else
                total_width += 1;
        }
        std::string tmp = "$ast_t" + std::to_string(tmp_cnt++);
        m.wires[tmp] = MyIR::Port{tmp, MyIR::PortDir::Internal, total_width, false};
        m.exprs.push_back(MyIR::Expr{MyIR::OpType::Concat, tmp, total_width, parts});
        return tmp;
    }

    log_warning("myir_ast_import: unhandled expression type %d -- using 1'0.\n",
                (int)node->type);
    return "1'0";
}

// Flatten the RHS of a continuous assign, setting result_wire to lhs_name
// on the last generated Expr (avoids a redundant intermediate wire).
static void flatten_assign(AstNode *lhs_node, AstNode *rhs_node,
                           MyIR::Module &m, int &tmp_cnt)
{
    std::string lhs_name = lhs_node->type == AST::AST_IDENTIFIER
                               ? lhs_node->str : flatten_expr(lhs_node, m, tmp_cnt);
    std::string rhs_name = flatten_expr(rhs_node, m, tmp_cnt);

    // If the RHS flattening produced a last Expr with a temp result,
    // rewrite that Expr's result_wire to the LHS directly.
    if (!m.exprs.empty() && m.exprs.back().result_wire == rhs_name &&
        rhs_name.substr(0, 6) == "$ast_t")
    {
        auto &last = m.exprs.back();
        // Remove the temp wire, use lhs wire instead
        m.wires.erase(rhs_name);
        last.result_wire = lhs_name;
        auto lw = m.wires.find(lhs_name);
        if (lw != m.wires.end())
            last.result_width = lw->second.width;
    } else {
        m.connects.push_back(MyIR::Assign{lhs_name, rhs_name});
    }
}

// ================================================================
//  Statement extractor (for always block bodies)
// ================================================================

static MyIR::Statement extract_stmt(AstNode *node, MyIR::Module &m, int &tmp_cnt);

static std::vector<MyIR::Statement> extract_stmts(AstNode *block,
                                                    MyIR::Module &m, int &tmp_cnt)
{
    std::vector<MyIR::Statement> result;
    if (!block) return result;
    for (auto *child : block->children) {
        // Skip sensitivity-list items at the top of always
        if (child->type == AST::AST_POSEDGE ||
            child->type == AST::AST_NEGEDGE ||
            child->type == AST::AST_EDGE)
            continue;
        result.push_back(extract_stmt(child, m, tmp_cnt));
    }
    return result;
}

static MyIR::Statement extract_stmt(AstNode *node, MyIR::Module &m, int &tmp_cnt)
{
    MyIR::Statement s;

    switch (node->type)
    {
    case AST::AST_BLOCK:
    {
        s.kind = MyIR::Statement::Block;
        s.block_body = extract_stmts(node, m, tmp_cnt);
        return s;
    }

    case AST::AST_ASSIGN_EQ:
    case AST::AST_ASSIGN_LE:
    {
        s.kind = MyIR::Statement::Assign;
        s.proc_assign.is_blocking = (node->type == AST::AST_ASSIGN_EQ);
        if (node->children.size() >= 2) {
            s.proc_assign.lhs = flatten_expr(node->children[0], m, tmp_cnt);
            s.proc_assign.rhs = flatten_expr(node->children[1], m, tmp_cnt);
        }
        return s;
    }

    case AST::AST_CASE:
    {
        if (node->children.empty()) break;

        // children[0] = select/condition expression
        std::string sel = flatten_expr(node->children[0], m, tmp_cnt);

        // Count AST_COND children
        std::vector<AstNode*> conds;
        for (size_t i = 1; i < node->children.size(); i++)
            if (node->children[i]->type == AST::AST_COND ||
                node->children[i]->type == AST::AST_CONDX ||
                node->children[i]->type == AST::AST_CONDZ)
                conds.push_back(node->children[i]);

        // Detect if this is an if/else pattern:
        // 2 CONDs where the second has AST_DEFAULT as first child
        bool is_if_else = false;
        if (conds.size() == 2 && !conds[1]->children.empty() &&
            conds[1]->children[0]->type == AST::AST_DEFAULT)
            is_if_else = true;
        // Also: single COND = pure if (no else)
        if (conds.size() == 1)
            is_if_else = true;

        if (is_if_else) {
            s.kind = MyIR::Statement::If;
            s.if_else.condition = sel;

            // then-branch: body of first COND (skip compare-value children)
            if (!conds.empty()) {
                for (auto *ch : conds[0]->children) {
                    if (ch->type == AST::AST_BLOCK)
                        s.if_else.then_body = extract_stmts(ch, m, tmp_cnt);
                    else if (ch->type != AST::AST_CONSTANT &&
                             ch->type != AST::AST_DEFAULT &&
                             ch->type != AST::AST_IDENTIFIER) {
                        s.if_else.then_body.push_back(extract_stmt(ch, m, tmp_cnt));
                    }
                }
            }
            // else-branch
            if (conds.size() >= 2) {
                for (auto *ch : conds[1]->children) {
                    if (ch->type == AST::AST_DEFAULT) continue;
                    if (ch->type == AST::AST_BLOCK)
                        s.if_else.else_body = extract_stmts(ch, m, tmp_cnt);
                    else
                        s.if_else.else_body.push_back(extract_stmt(ch, m, tmp_cnt));
                }
            }
            return s;
        }

        // General case statement
        s.kind = MyIR::Statement::Case;
        s.case_stmt.select = sel;

        for (auto *cond_node : conds) {
            MyIR::CaseItem item;
            for (auto *ch : cond_node->children) {
                if (ch->type == AST::AST_DEFAULT) {
                    // default branch: values stays empty
                } else if (ch->type == AST::AST_CONSTANT) {
                    item.values.push_back(bits_to_const_name(ch->bits));
                } else if (ch->type == AST::AST_BLOCK) {
                    item.body = extract_stmts(ch, m, tmp_cnt);
                } else {
                    item.body.push_back(extract_stmt(ch, m, tmp_cnt));
                }
            }
            s.case_stmt.items.push_back(item);
        }
        return s;
    }

    default:
        break;
    }

    // Fallback: unknown statement
    s.kind = MyIR::Statement::Assign;
    s.proc_assign.lhs = "?";
    s.proc_assign.rhs = "?";
    log_warning("myir_ast_import: unhandled statement type %d.\n", (int)node->type);
    return s;
}

// ================================================================
//  The Pass
// ================================================================

struct MyIRAstImportPass : public Pass {
    MyIRAstImportPass() : Pass("myir_ast_import",
        "convert Verilog AST to MyIR representation") {}

    void help() override
    {
        log("\n");
        log("    myir_ast_import\n");
        log("\n");
        log("Convert the Verilog AST (attached to each module after read_verilog)\n");
        log("into the MyIR intermediate representation.\n");
        log("\n");
        log("Unlike myir_import (which reads from RTLIL), this pass walks the\n");
        log("original AST directly, preserving expression-tree structure and\n");
        log("procedural constructs (if/else, case, assignments).\n");
        log("\n");
        log("Run read_verilog before this pass.  The result is stored in the\n");
        log("global MyIR::Design and can be inspected with myir_dump.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_ast_import.\n");
        extra_args(args, 1, design);

        MyIR::ensure_design();
        MyIR::Design *myir = MyIR::get_design();
        myir->clear();

        for (auto module : design->selected_modules())
        {
            auto *amod = dynamic_cast<AST::AstModule*>(module);
            if (!amod || !amod->ast) {
                log_warning("Module '%s' has no AST (not from read_verilog?) "
                            "-- skipped.\n", module->name.c_str());
                continue;
            }

            AstNode *mod_ast = amod->ast;
            std::string mod_name = module->name.str();

            MyIR::Module &mymod = myir->modules[mod_name];
            mymod.name = mod_name;

            int tmp_cnt = 0;
            std::vector<std::pair<int, std::string>> port_order;

            // ---- Walk AST children ----
            for (auto *child : mod_ast->children)
            {
                switch (child->type)
                {
                // -- Wire / Port declaration --
                case AST::AST_WIRE:
                {
                    MyIR::Port p;
                    p.name      = child->str;
                    p.width     = extract_range_width(child);
                    p.is_signed = child->is_signed;

                    if (child->is_input && child->is_output)
                        p.dir = MyIR::PortDir::InOut;
                    else if (child->is_input)
                        p.dir = MyIR::PortDir::Input;
                    else if (child->is_output)
                        p.dir = MyIR::PortDir::Output;
                    else
                        p.dir = MyIR::PortDir::Internal;

                    mymod.wires[p.name] = p;

                    if (child->port_id > 0)
                        port_order.push_back({child->port_id, p.name});
                    break;
                }

                // -- Continuous assignment --
                case AST::AST_ASSIGN:
                {
                    if (child->children.size() >= 2)
                        flatten_assign(child->children[0],
                                       child->children[1],
                                       mymod, tmp_cnt);
                    break;
                }

                // -- Always block -> ProcBlock --
                case AST::AST_ALWAYS:
                {
                    MyIR::ProcBlock pb;
                    for (auto *ac : child->children) {
                        if (ac->type == AST::AST_POSEDGE) {
                            MyIR::SensItem si;
                            si.type = MyIR::SensItem::Posedge;
                            if (!ac->children.empty() &&
                                ac->children[0]->type == AST::AST_IDENTIFIER)
                                si.signal = ac->children[0]->str;
                            pb.sensitivity.push_back(si);
                        } else if (ac->type == AST::AST_NEGEDGE) {
                            MyIR::SensItem si;
                            si.type = MyIR::SensItem::Negedge;
                            if (!ac->children.empty() &&
                                ac->children[0]->type == AST::AST_IDENTIFIER)
                                si.signal = ac->children[0]->str;
                            pb.sensitivity.push_back(si);
                        } else if (ac->type == AST::AST_EDGE) {
                            MyIR::SensItem si;
                            si.type = MyIR::SensItem::Level;
                            if (!ac->children.empty() &&
                                ac->children[0]->type == AST::AST_IDENTIFIER)
                                si.signal = ac->children[0]->str;
                            pb.sensitivity.push_back(si);
                        } else if (ac->type == AST::AST_BLOCK) {
                            pb.body = extract_stmts(ac, mymod, tmp_cnt);
                        } else {
                            pb.body.push_back(extract_stmt(ac, mymod, tmp_cnt));
                        }
                    }
                    if (pb.sensitivity.empty())
                        pb.sensitivity.push_back(MyIR::SensItem{MyIR::SensItem::Star, ""});
                    mymod.proc_blocks.push_back(pb);
                    break;
                }

                // -- Parameter / localparam --
                case AST::AST_PARAMETER:
                case AST::AST_LOCALPARAM:
                {
                    if (!child->children.empty() &&
                        child->children[0]->type == AST::AST_CONSTANT) {
                        std::string cval = bits_to_const_name(
                            child->children[0]->bits);
                        MyIR::Port p;
                        p.name  = child->str;
                        p.width = (int)child->children[0]->bits.size();
                        p.dir   = MyIR::PortDir::Internal;
                        mymod.wires[p.name] = p;
                        mymod.connects.push_back(MyIR::Assign{p.name, cval});
                    }
                    break;
                }

                default:
                    log("  (skipping AST node type %d: '%s')\n",
                        (int)child->type, child->str.c_str());
                    break;
                }
            }

            // Sort ports by port_id
            std::sort(port_order.begin(), port_order.end());
            for (auto &pp : port_order) {
                auto it = mymod.wires.find(pp.second);
                if (it != mymod.wires.end())
                    mymod.ports.push_back(it->second);
            }

            log("  Imported module '%s' from AST: %d ports, %d wires, "
                "%d exprs, %d connects, %d proc_blocks.\n",
                mod_name.c_str(),
                (int)mymod.ports.size(),
                (int)mymod.wires.size(),
                (int)mymod.exprs.size(),
                (int)mymod.connects.size(),
                (int)mymod.proc_blocks.size());
        }
    }
} MyIRAstImportPass;

PRIVATE_NAMESPACE_END
