#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "passes/myir/myir.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static std::string sigspec_to_name(const RTLIL::SigSpec &sig)
{
    if (sig.is_wire())
        return sig.as_wire()->name.str();

    if (sig.is_fully_const()) {
        int w = sig.size();
        std::string bits;
        for (int i = w - 1; i >= 0; i--) {
            RTLIL::SigBit bit = sig[i];
            bits += (bit == RTLIL::State::S1) ? '1' : '0';
        }
        return std::to_string(w) + "'" + bits;
    }

    if (sig.chunks().size() == 1) {
        const RTLIL::SigChunk &chunk = sig.chunks()[0];
        if (chunk.wire != nullptr)
            return chunk.wire->name.str();
    }

    return std::string(log_signal(sig));
}

struct MyIRImportPass : public Pass {
    MyIRImportPass() : Pass("myir_import",
        "convert RTLIL design to MyIR representation") {}

    void help() override
    {
        log("\n");
        log("    myir_import\n");
        log("\n");
        log("Convert the current RTLIL design into the MyIR intermediate\n");
        log("representation.  The result is stored in a global MyIR::Design\n");
        log("and can be inspected with myir_dump, optimised with myir_opt_*,\n");
        log("or exported back to RTLIL with myir_export.\n");
        log("\n");
        log("Tip: run 'proc' before myir_import if the design contains\n");
        log("always-blocks or other procedural constructs.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_import.\n");
        extra_args(args, 1, design);

        MyIR::ensure_design();
        MyIR::Design *myir = MyIR::get_design();
        myir->clear();

        for (auto module : design->selected_modules())
        {
            std::string mod_name = module->name.str();
            MyIR::Module &mymod  = myir->modules[mod_name];
            mymod.name = mod_name;

            // -- wires & ports ------------------------------------------------
            std::vector<std::pair<int, std::string>> port_order;

            for (auto wire : module->wires())
            {
                MyIR::Port p;
                p.name      = wire->name.str();
                p.width     = wire->width;
                p.is_signed = wire->is_signed;

                if (wire->port_input && wire->port_output)
                    p.dir = MyIR::PortDir::InOut;
                else if (wire->port_input)
                    p.dir = MyIR::PortDir::Input;
                else if (wire->port_output)
                    p.dir = MyIR::PortDir::Output;
                else
                    p.dir = MyIR::PortDir::Internal;

                mymod.wires[p.name] = p;

                if (wire->port_id > 0)
                    port_order.push_back({wire->port_id, p.name});
            }

            std::sort(port_order.begin(), port_order.end());
            for (auto &pp : port_order)
                mymod.ports.push_back(mymod.wires.at(pp.second));

            // -- cells -> expressions -----------------------------------------
            for (auto cell : module->cells())
            {
                std::string type_str = cell->type.str();
                MyIR::OpType op = MyIR::celltype_to_optype(type_str);

                if (op == MyIR::OpType::Unknown) {
                    log_warning("myir_import: unsupported cell type '%s' "
                                "(cell '%s') -- skipped.\n",
                                type_str.c_str(), cell->name.c_str());
                    continue;
                }

                MyIR::Expr expr;
                expr.op = op;

                if (cell->hasPort(ID::Y)) {
                    RTLIL::SigSpec y_sig = cell->getPort(ID::Y);
                    expr.result_wire  = sigspec_to_name(y_sig);
                    expr.result_width = y_sig.size();
                }

                if (cell->hasPort(ID::A))
                    expr.operands.push_back(sigspec_to_name(cell->getPort(ID::A)));

                if (op == MyIR::OpType::Mux) {
                    if (cell->hasPort(ID::B))
                        expr.operands.push_back(sigspec_to_name(cell->getPort(ID::B)));
                    if (cell->hasPort(ID::S))
                        expr.operands.push_back(sigspec_to_name(cell->getPort(ID::S)));
                } else {
                    int expected = MyIR::optype_num_inputs(op);
                    if (expected >= 2 && cell->hasPort(ID::B))
                        expr.operands.push_back(sigspec_to_name(cell->getPort(ID::B)));
                }

                mymod.exprs.push_back(expr);
            }

            // -- connections -> assigns ---------------------------------------
            for (auto &conn : module->connections())
            {
                MyIR::Assign a;
                a.lhs = sigspec_to_name(conn.first);
                a.rhs = sigspec_to_name(conn.second);
                mymod.connects.push_back(a);
            }

            log("  Imported module '%s': %d ports, %d wires, "
                "%d exprs, %d connects.\n",
                mod_name.c_str(),
                (int)mymod.ports.size(),
                (int)mymod.wires.size(),
                (int)mymod.exprs.size(),
                (int)mymod.connects.size());
        }
    }
} MyIRImportPass;

PRIVATE_NAMESPACE_END
