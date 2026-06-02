#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Build a wire -> constant-name map by transitively walking m.connects
// and single-operand Buf/Pos expressions in m.exprs.  This lets
// propagation see through `assign y = x` style copies whether they
// landed as a connect or as a Buf/Pos cell.
//
// On return *derived_via_buf_pos receives the number of entries
// inferred from Buf/Pos expressions (the rest came from connects).
static std::unordered_map<std::string, std::string>
build_known_constants(const MyIR::Module &m,
                      int *derived_via_buf_pos = nullptr)
{
    std::unordered_map<std::string, std::string> known_consts;
    int buf_pos_hits = 0;
    bool changed = true;

    while (changed) {
        changed = false;

        for (const auto &c : m.connects) {
            std::string rhs = c.rhs;
            auto rhs_it = known_consts.find(rhs);
            if (rhs_it != known_consts.end())
                rhs = rhs_it->second;

            if (!MyIR::is_constant_name(rhs))
                continue;

            auto lhs_it = known_consts.find(c.lhs);
            if (lhs_it == known_consts.end() || lhs_it->second != rhs) {
                known_consts[c.lhs] = rhs;
                changed = true;
            }
        }

        for (const auto &e : m.exprs) {
            if ((e.op != MyIR::OpType::Buf && e.op != MyIR::OpType::Pos)
                || e.operands.size() != 1)
                continue;

            std::string rhs = e.operands[0];
            auto rhs_it = known_consts.find(rhs);
            if (rhs_it != known_consts.end())
                rhs = rhs_it->second;

            if (!MyIR::is_constant_name(rhs))
                continue;

            auto lhs_it = known_consts.find(e.result_wire);
            if (lhs_it == known_consts.end() || lhs_it->second != rhs) {
                if (lhs_it == known_consts.end())
                    buf_pos_hits++;
                known_consts[e.result_wire] = rhs;
                changed = true;
            }
        }
    }

    if (derived_via_buf_pos)
        *derived_via_buf_pos = buf_pos_hits;

    return known_consts;
}

struct MyIROptConstPass : public Pass {
    MyIROptConstPass() : Pass("iris_opt_const",
        "constant propagation on MyIR") {}

    void help() override
    {
        log("\n");
        log("    iris_opt_const\n");
        log("\n");
        log("Propagate constants through direct MyIR assignments.\n");
        log("Sees through both `connect` records and single-operand Buf / Pos\n");
        log("expressions (i.e. `assign y = x` style copies).  Expression\n");
        log("folding itself is handled by iris_opt_expr.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing iris_opt_const.\n");
        extra_args(args, 1, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded.\n");
            return;
        }

        int modules_visited = 0;
        int total_known_wires = 0;
        int total_buf_pos_hits = 0;
        int total_operand_subs = 0;
        int total_connect_subs = 0;

        for (auto &mkv : myir->modules)
        {
            MyIR::Module &m = mkv.second;
            modules_visited++;
            bool changed = true;
            int buf_pos_hits = 0;
            std::unordered_map<std::string, std::string> known_consts;

            while (changed) {
                changed = false;
                known_consts = build_known_constants(m, &buf_pos_hits);

                for (auto &c : m.connects) {
                    auto rhs_it = known_consts.find(c.rhs);
                    if (rhs_it != known_consts.end() && c.rhs != rhs_it->second) {
                        log("  [opt_const] propagated %s -> %s through connect in module '%s'\n",
                            c.rhs.c_str(), rhs_it->second.c_str(), m.name.c_str());
                        c.rhs = rhs_it->second;
                        total_connect_subs++;
                        changed = true;
                    }
                }

                for (auto &e : m.exprs)
                    for (auto &op : e.operands) {
                        auto it = known_consts.find(op);
                        if (it == known_consts.end() || op == it->second)
                            continue;

                        log("  [opt_const] propagated %s -> %s in module '%s'\n",
                            op.c_str(), it->second.c_str(), m.name.c_str());
                        op = it->second;
                        total_operand_subs++;
                        changed = true;
                    }
            }

            total_known_wires += (int)known_consts.size();
            total_buf_pos_hits += buf_pos_hits;
        }

        log("  iris_opt_const summary:\n");
        log("    modules visited        : %d\n", modules_visited);
        log("    known-constant wires   : %d (incl. %d derived via Buf/Pos)\n",
            total_known_wires, total_buf_pos_hits);
        log("    operand replacements   : %d\n", total_operand_subs);
        log("    connect rewrites       : %d\n", total_connect_subs);
        log("    total substitutions    : %d\n",
            total_operand_subs + total_connect_subs);
    }
} MyIROptConstPass;

PRIVATE_NAMESPACE_END
