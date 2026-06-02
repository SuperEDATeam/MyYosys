#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <set>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct MyIROptCleanPass : public Pass {
    MyIROptCleanPass() : Pass("iris_opt_clean",
        "dead-code elimination on MyIR") {}

    void help() override
    {
        log("\n");
        log("    iris_opt_clean\n");
        log("\n");
        log("Remove expressions whose result wires are never referenced by\n");
        log("any other expression, connection, or output port.  The\n");
        log("corresponding internal wires are also removed.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing iris_opt_clean.\n");
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

                // collect all referenced wires
                std::set<std::string> used;

                // output / inout ports are always live
                for (auto &wkv : m.wires) {
                    MyIR::PortDir d = wkv.second.dir;
                    if (d == MyIR::PortDir::Output || d == MyIR::PortDir::InOut)
                        used.insert(wkv.first);
                }

                // operands of all expressions reference wires
                for (auto &e : m.exprs)
                    for (auto &op : e.operands)
                        used.insert(op);

                // connections
                for (auto &c : m.connects) {
                    used.insert(c.rhs);
                    used.insert(c.lhs);
                }

                // remove dead expressions
                std::vector<MyIR::Expr> alive;
                for (auto &e : m.exprs) {
                    if (used.count(e.result_wire)) {
                        alive.push_back(e);
                    } else {
                        log("  [opt_clean] removed dead expr: %s -> %s\n",
                            MyIR::optype_to_string(e.op),
                            e.result_wire.c_str());
                        ++total;
                        changed = true;
                    }
                }
                m.exprs = alive;

                // remove dead internal wires
                std::set<std::string> all_used_wires;
                for (auto &e : m.exprs) {
                    all_used_wires.insert(e.result_wire);
                    for (auto &op : e.operands)
                        all_used_wires.insert(op);
                }
                for (auto &c : m.connects) {
                    all_used_wires.insert(c.lhs);
                    all_used_wires.insert(c.rhs);
                }
                for (auto &p : m.ports)
                    all_used_wires.insert(p.name);

                std::vector<std::string> dead_wires;
                for (auto &wkv : m.wires)
                    if (wkv.second.dir == MyIR::PortDir::Internal &&
                        !all_used_wires.count(wkv.first))
                        dead_wires.push_back(wkv.first);

                for (auto &dw : dead_wires) {
                    log("  [opt_clean] removed dead wire: %s\n", dw.c_str());
                    m.wires.erase(dw);
                }
            }
        }

        log("  Removed %d dead expression(s).\n", total);
    }
} MyIROptCleanPass;

PRIVATE_NAMESPACE_END
