#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static std::string expr_key(const MyIR::Expr &e)
{
    std::string k = MyIR::optype_to_string(e.op);
    k += '(';
    for (size_t i = 0; i < e.operands.size(); i++) {
        if (i) k += ',';
        k += e.operands[i];
    }
    k += ')';
    return k;
}

static void replace_operand(MyIR::Module &m,
                             const std::string &old_wire,
                             const std::string &new_wire)
{
    for (auto &e : m.exprs)
        for (auto &op : e.operands)
            if (op == old_wire) op = new_wire;

    for (auto &c : m.connects) {
        if (c.rhs == old_wire) c.rhs = new_wire;
        if (c.lhs == old_wire) c.lhs = new_wire;
    }
}

struct MyIROptSharePass : public Pass {
    MyIROptSharePass() : Pass("iris_opt_share",
        "shared sub-expression elimination on MyIR") {}

    void help() override
    {
        log("\n");
        log("    iris_opt_share\n");
        log("\n");
        log("Identify expressions with identical operators and operands and\n");
        log("merge them so that only one computation is kept.  All later\n");
        log("references are rewritten to use the shared result wire.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing iris_opt_share.\n");
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
            // key -> result_wire of the first occurrence
            std::unordered_map<std::string, std::string> seen;
            std::vector<MyIR::Expr> kept;

            for (auto &e : m.exprs) {
                std::string k = expr_key(e);
                auto it = seen.find(k);
                if (it == seen.end()) {
                    seen[k] = e.result_wire;
                    kept.push_back(e);
                } else {
                    // duplicate -- rewrite all uses of e.result_wire
                    log("  [opt_share] '%s' is duplicate of '%s' "
                        "-> reuse wire '%s'.\n",
                        e.result_wire.c_str(),
                        it->second.c_str(),
                        it->second.c_str());
                    replace_operand(m, e.result_wire, it->second);
                    ++total;
                }
            }
            m.exprs = kept;
        }

        log("  Eliminated %d redundant expression(s).\n", total);
    }
} MyIROptSharePass;

PRIVATE_NAMESPACE_END
