#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static std::unordered_map<std::string, std::string>
build_known_constants(const MyIR::Module &m)
{
    std::unordered_map<std::string, std::string> known_consts;
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
    }

    return known_consts;
}

struct MyIROptConstPass : public Pass {
    MyIROptConstPass() : Pass("myir_opt_const",
        "constant propagation on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_const\n");
        log("\n");
        log("Propagate constants through direct MyIR assignments.\n");
        log("This pass no longer performs expression folding; that logic is\n");
        log("handled by myir_opt_expr.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_const.\n");
        extra_args(args, 1, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded.\n");
            return;
        }

        int total = 0;

        for (auto &mkv : myir->modules)
        {
            MyIR::Module &m = mkv.second;
            bool changed = true;

            while (changed) {
                changed = false;
                auto known_consts = build_known_constants(m);

                for (auto &c : m.connects) {
                    auto rhs_it = known_consts.find(c.rhs);
                    if (rhs_it != known_consts.end() && c.rhs != rhs_it->second) {
                        log("  [opt_const] propagated %s -> %s through connect in module '%s'\n",
                            c.rhs.c_str(), rhs_it->second.c_str(), m.name.c_str());
                        c.rhs = rhs_it->second;
                        total++;
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
                        total++;
                        changed = true;
                    }
            }
        }

        log("  Propagated %d constant operand substitution(s).\n", total);
    }
} MyIROptConstPass;

PRIVATE_NAMESPACE_END
