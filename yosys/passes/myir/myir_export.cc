#include "kernel/yosys.h"
#include "passes/myir/myir.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Normalize a MyIR wire/port name into a valid RTLIL::IdString.
// Yosys requires non-anonymous identifiers to start with '\\'; anonymous
// auto-generated names start with '$'.  MyIR names may come from
// continuous-assign LHS strings or temporary "$ast_t<N>" names; both
// must be passed unchanged, but plain "n123" / "count[0]" style names
// need a '\\' prefix prepended (otherwise IdString hits an assert).
static RTLIL::IdString to_id(const std::string &name)
{
    if (name.empty()) return RTLIL::IdString();
    if (name[0] == '\\' || name[0] == '$') return RTLIL::IdString(name);
    return RTLIL::IdString("\\" + name);
}

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

    RTLIL::Wire *wire = mod->wire(to_id(name));
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
        log("    myir_export [-strict]\n");
        log("\n");
        log("Convert the current MyIR::Design back into RTLIL modules.\n");
        log("Existing RTLIL modules with the same names are replaced.\n");
        log("\n");
        log("Options:\n");
        log("\n");
        log("    -strict\n");
        log("        Promote silent drops to hard errors.  The default\n");
        log("        (best-effort) mode keeps the historical behaviour\n");
        log("        of skipping anything that cannot be expressed as\n");
        log("        an RTLIL cell, but counts every drop in the summary.\n");
        log("        With -strict, the following situations all call\n");
        log("        log_error and abort the pass:\n");
        log("\n");
        log("          - any leftover proc_block in a module\n");
        log("            (myir_ast_import did not fully lower an\n");
        log("             always/initial block)\n");
        log("          - any Expr with OpType::Unknown\n");
        log("          - any Expr whose OpType has no $-cell mapping\n");
        log("            in optype_to_celltype()\n");
        log("\n");
        log("End-of-pass summary fields:\n");
        log("    modules exported       : modules written to RTLIL\n");
        log("    dropped proc_blocks    : skipped procedural blocks\n");
        log("    dropped Unknown exprs  : exprs with OpType::Unknown\n");
        log("    dropped no-celltype    : exprs with no $-cell mapping\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_export.\n");

        bool flag_strict = false;
        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            const std::string &a = args[argidx];
            if (a == "-strict") { flag_strict = true; continue; }
            break;
        }
        extra_args(args, argidx, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty())
            log_error("No MyIR design loaded. Run myir_import first.\n");

        int dropped_proc       = 0;
        int dropped_unknown    = 0;
        int dropped_celltype   = 0;
        int modules_exported   = 0;

        for (auto &modkv : myir->modules)
        {
            const MyIR::Module &mymod = modkv.second;
            RTLIL::IdString mod_id = to_id(mymod.name);

            if (design->module(mod_id))
                design->remove(design->module(mod_id));

            RTLIL::Module *mod = design->addModule(mod_id);
            modules_exported++;

            // -- create wires -------------------------------------------------
            for (auto &wkv : mymod.wires) {
                const MyIR::Port &p = wkv.second;
                if (MyIR::is_constant_name(p.name))
                    continue;

                RTLIL::Wire *wire = mod->addWire(to_id(p.name), p.width);
                wire->is_signed = p.is_signed;

                if (p.dir == MyIR::PortDir::Input)       wire->port_input  = true;
                else if (p.dir == MyIR::PortDir::Output)  wire->port_output = true;
                else if (p.dir == MyIR::PortDir::InOut)  { wire->port_input = true;
                                                            wire->port_output = true; }
            }

            int port_id = 1;
            for (auto &port : mymod.ports) {
                RTLIL::Wire *wire = mod->wire(to_id(port.name));
                if (wire) wire->port_id = port_id++;
            }
            mod->fixup_ports();

            // -- create cells from expressions --------------------------------
            int cell_cnt = 0;
            for (auto &expr : mymod.exprs)
            {
                if (expr.op == MyIR::OpType::Unknown) {
                    dropped_unknown++;
                    if (flag_strict)
                        log_error("myir_export -strict: Expr -> '%s' in module "
                                  "'%s' has OpType::Unknown.\n",
                                  expr.result_wire.c_str(),
                                  mymod.name.c_str());
                    continue;
                }
                std::string celltype = MyIR::optype_to_celltype(expr.op);
                if (celltype.empty()) {
                    dropped_celltype++;
                    if (flag_strict)
                        log_error("myir_export -strict: OpType %s in module "
                                  "'%s' has no $-cell mapping in "
                                  "optype_to_celltype().\n",
                                  MyIR::optype_to_string(expr.op),
                                  mymod.name.c_str());
                    continue;
                }

                std::string cname = "$myir_c" + std::to_string(cell_cnt++);
                RTLIL::Cell *cell = mod->addCell(RTLIL::IdString(cname),
                                                  RTLIL::IdString(celltype));

                // The $mux cell has a fundamentally different parameter
                // schema: it only takes \WIDTH, NOT the
                // A_/B_/Y_ (WIDTH | SIGNED) quadruple that the generic
                // $-binary/$-unary cells expect.  Setting them anyway makes
                // rtlil.cc:1091 reject the cell at the next pass.
                if (expr.op == MyIR::OpType::Mux) {
                    cell->setPort(ID::Y, name_to_sigspec(expr.result_wire, mod));
                    if (expr.operands.size() >= 1)
                        cell->setPort(ID::A,
                                      name_to_sigspec(expr.operands[0], mod));
                    if (expr.operands.size() >= 2)
                        cell->setPort(ID::B,
                                      name_to_sigspec(expr.operands[1], mod));
                    if (expr.operands.size() >= 3)
                        cell->setPort(ID::S,
                                      name_to_sigspec(expr.operands[2], mod));
                    cell->parameters[ID::WIDTH] = RTLIL::Const(expr.result_width);
                    continue;
                }

                // Generic $-cell path (unary + binary): A/B/Y plus the
                // matching *_WIDTH / *_SIGNED parameters.
                RTLIL::SigSpec y = name_to_sigspec(expr.result_wire, mod);
                cell->setPort(ID::Y, y);

                // Force Y_WIDTH = 1 for cells whose RTLIL semantics
                // mandate a 1-bit output (compare/reduce/logic ops),
                // regardless of what result_width happens to carry.
                int y_w = expr.result_width;
                if (MyIR::op_returns_1bit(expr.op))
                    y_w = 1;
                cell->parameters[ID::Y_WIDTH] = RTLIL::Const(y_w);

                if (!expr.operands.empty()) {
                    RTLIL::SigSpec a = name_to_sigspec(expr.operands[0], mod);
                    cell->setPort(ID::A, a);
                    cell->parameters[ID::A_WIDTH] = RTLIL::Const(a.size());

                    bool a_signed = false;
                    auto aw = mymod.wires.find(expr.operands[0]);
                    if (aw != mymod.wires.end()) a_signed = aw->second.is_signed;
                    cell->parameters[ID::A_SIGNED] = RTLIL::Const(a_signed ? 1 : 0);
                }

                if (!is_unary(expr.op) && expr.operands.size() >= 2) {
                    RTLIL::SigSpec b = name_to_sigspec(expr.operands[1], mod);
                    cell->setPort(ID::B, b);
                    cell->parameters[ID::B_WIDTH] = RTLIL::Const(b.size());

                    bool b_signed = false;
                    auto bw = mymod.wires.find(expr.operands[1]);
                    if (bw != mymod.wires.end()) b_signed = bw->second.is_signed;
                    cell->parameters[ID::B_SIGNED] = RTLIL::Const(b_signed ? 1 : 0);
                }
            }

            // -- create connections from assigns ------------------------------
            for (auto &assign : mymod.connects) {
                RTLIL::SigSpec lhs = name_to_sigspec(assign.lhs, mod);
                RTLIL::SigSpec rhs = name_to_sigspec(assign.rhs, mod);
                mod->connect(lhs, rhs);
            }

            if (!mymod.proc_blocks.empty()) {
                dropped_proc += (int)mymod.proc_blocks.size();
                if (flag_strict)
                    log_error("myir_export -strict: module '%s' still has %d "
                              "proc_block(s); AST import did not fully lower "
                              "always/initial blocks.\n",
                              mymod.name.c_str(),
                              (int)mymod.proc_blocks.size());
                log_warning("Module '%s' contains %d procedural block(s) "
                            "from AST import; these are NOT exported to "
                            "RTLIL (only exprs/connects are exported).\n",
                            mymod.name.c_str(),
                            (int)mymod.proc_blocks.size());
            }

            log("  Exported module '%s': %d wires, %d cells, "
                "%d connections.\n",
                mymod.name.c_str(),
                (int)mod->wires_.size(),
                (int)mod->cells_.size(),
                (int)mod->connections_.size());
        }

        // -- final summary ---------------------------------------------------
        log("\n  myir_export summary:\n");
        log("    modules exported       : %d\n", modules_exported);
        log("    dropped proc_blocks    : %d\n", dropped_proc);
        log("    dropped Unknown exprs  : %d\n", dropped_unknown);
        log("    dropped no-celltype    : %d\n", dropped_celltype);
        if (!flag_strict &&
            (dropped_proc || dropped_unknown || dropped_celltype))
            log("    (re-run with -strict to fail loud on any drop)\n");
    }
} MyIRExportPass;

PRIVATE_NAMESPACE_END
