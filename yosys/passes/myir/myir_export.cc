#include "kernel/yosys.h"
#include "passes/myir/myir.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static RTLIL::SigSpec name_to_sigspec(const std::string &name,
                                       RTLIL::Module *mod)
{
    if (MyIR::is_constant_name(name)) {
        size_t pos = name.find('\'');
        int width = std::stoi(name.substr(0, pos));
        std::string bits_str = name.substr(pos + 1);
        std::vector<RTLIL::State> bits;
        for (int i = (int)bits_str.size() - 1; i >= 0; i--)
            bits.push_back(bits_str[i] == '1' ? RTLIL::State::S1
                                               : RTLIL::State::S0);
        while ((int)bits.size() < width)
            bits.push_back(RTLIL::State::S0);
        return RTLIL::SigSpec(RTLIL::Const(bits));
    }

    RTLIL::Wire *wire = mod->wire(RTLIL::IdString(name));
    if (wire)
        return RTLIL::SigSpec(wire);

    log_error("myir_export: wire '%s' not found in module '%s'.\n",
              name.c_str(), mod->name.c_str());
    return RTLIL::SigSpec();
}

static bool is_unary(MyIR::OpType op) {
    return MyIR::optype_num_inputs(op) == 1;
}

struct MyIRExportPass : public Pass {
    MyIRExportPass() : Pass("myir_export",
        "convert MyIR representation back to RTLIL") {}

    void help() override
    {
        log("\n");
        log("    myir_export\n");
        log("\n");
        log("Convert the current MyIR::Design back into RTLIL modules.\n");
        log("Existing RTLIL modules with the same names are replaced.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_export.\n");
        extra_args(args, 1, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty())
            log_error("No MyIR design loaded. Run myir_import first.\n");

        for (auto &modkv : myir->modules)
        {
            const MyIR::Module &mymod = modkv.second;
            RTLIL::IdString mod_id(mymod.name);

            if (design->module(mod_id))
                design->remove(design->module(mod_id));

            RTLIL::Module *mod = design->addModule(mod_id);

            // -- create wires -------------------------------------------------
            for (auto &wkv : mymod.wires) {
                const MyIR::Port &p = wkv.second;
                if (MyIR::is_constant_name(p.name))
                    continue;

                RTLIL::Wire *wire = mod->addWire(RTLIL::IdString(p.name), p.width);
                wire->is_signed = p.is_signed;

                if (p.dir == MyIR::PortDir::Input)       wire->port_input  = true;
                else if (p.dir == MyIR::PortDir::Output)  wire->port_output = true;
                else if (p.dir == MyIR::PortDir::InOut)  { wire->port_input = true;
                                                            wire->port_output = true; }
            }

            int port_id = 1;
            for (auto &port : mymod.ports) {
                RTLIL::Wire *wire = mod->wire(RTLIL::IdString(port.name));
                if (wire) wire->port_id = port_id++;
            }
            mod->fixup_ports();

            // -- create cells from expressions --------------------------------
            int cell_cnt = 0;
            for (auto &expr : mymod.exprs)
            {
                if (expr.op == MyIR::OpType::Unknown) continue;
                std::string celltype = MyIR::optype_to_celltype(expr.op);
                if (celltype.empty()) continue;

                std::string cname = "$myir_c" + std::to_string(cell_cnt++);
                RTLIL::Cell *cell = mod->addCell(RTLIL::IdString(cname),
                                                  RTLIL::IdString(celltype));

                // Y port
                RTLIL::SigSpec y = name_to_sigspec(expr.result_wire, mod);
                cell->setPort(ID::Y, y);
                cell->parameters[ID::Y_WIDTH] = RTLIL::Const(expr.result_width);

                // A port
                if (!expr.operands.empty()) {
                    RTLIL::SigSpec a = name_to_sigspec(expr.operands[0], mod);
                    cell->setPort(ID::A, a);
                    cell->parameters[ID::A_WIDTH] = RTLIL::Const(a.size());

                    bool a_signed = false;
                    auto aw = mymod.wires.find(expr.operands[0]);
                    if (aw != mymod.wires.end()) a_signed = aw->second.is_signed;
                    cell->parameters[ID::A_SIGNED] = RTLIL::Const(a_signed ? 1 : 0);
                }

                if (expr.op == MyIR::OpType::Mux) {
                    // B port (input when S=1)
                    if (expr.operands.size() >= 2)
                        cell->setPort(ID::B,
                                      name_to_sigspec(expr.operands[1], mod));
                    // S port (select)
                    if (expr.operands.size() >= 3)
                        cell->setPort(ID::S,
                                      name_to_sigspec(expr.operands[2], mod));
                    cell->parameters[ID::WIDTH] = RTLIL::Const(expr.result_width);
                } else if (!is_unary(expr.op)) {
                    if (expr.operands.size() >= 2) {
                        RTLIL::SigSpec b = name_to_sigspec(expr.operands[1], mod);
                        cell->setPort(ID::B, b);
                        cell->parameters[ID::B_WIDTH] = RTLIL::Const(b.size());

                        bool b_signed = false;
                        auto bw = mymod.wires.find(expr.operands[1]);
                        if (bw != mymod.wires.end()) b_signed = bw->second.is_signed;
                        cell->parameters[ID::B_SIGNED] = RTLIL::Const(b_signed ? 1 : 0);
                    }
                }
            }

            // -- create connections from assigns ------------------------------
            for (auto &assign : mymod.connects) {
                RTLIL::SigSpec lhs = name_to_sigspec(assign.lhs, mod);
                RTLIL::SigSpec rhs = name_to_sigspec(assign.rhs, mod);
                mod->connect(lhs, rhs);
            }

            if (!mymod.proc_blocks.empty())
                log_warning("Module '%s' contains %d procedural block(s) "
                            "from AST import; these are NOT exported to "
                            "RTLIL (only exprs/connects are exported).\n",
                            mymod.name.c_str(),
                            (int)mymod.proc_blocks.size());

            log("  Exported module '%s': %d wires, %d cells, "
                "%d connections.\n",
                mymod.name.c_str(),
                (int)mod->wires_.size(),
                (int)mod->cells_.size(),
                (int)mod->connections_.size());
        }
    }
} MyIRExportPass;

PRIVATE_NAMESPACE_END
