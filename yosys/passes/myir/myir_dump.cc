#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static const char* portdir_str(MyIR::PortDir d)
{
    switch (d) {
    case MyIR::PortDir::Input:    return "input ";
    case MyIR::PortDir::Output:   return "output";
    case MyIR::PortDir::InOut:    return "inout ";
    case MyIR::PortDir::Internal: return "wire  ";
    }
    return "wire  ";
}

static std::string pretty_name(const std::string &s)
{
    if (!s.empty() && (s[0] == '\\' || s[0] == '$'))
        return s.substr(1);
    return s;
}

static void dump_stmt(std::ostream &os, const MyIR::Statement &s, int indent);

static void dump_stmts(std::ostream &os,
                        const std::vector<MyIR::Statement> &stmts,
                        int indent)
{
    for (auto &s : stmts)
        dump_stmt(os, s, indent);
}

static std::string ind(int n) { return std::string(n, ' '); }

static void dump_stmt(std::ostream &os, const MyIR::Statement &s, int indent)
{
    switch (s.kind) {
    case MyIR::Statement::Assign:
        os << ind(indent) << "assign "
           << pretty_name(s.proc_assign.lhs) << " "
           << (s.proc_assign.is_blocking ? "=" : "<=") << " "
           << pretty_name(s.proc_assign.rhs) << "\n";
        break;

    case MyIR::Statement::If:
        os << ind(indent) << "if (" << pretty_name(s.if_else.condition) << ")\n";
        dump_stmts(os, s.if_else.then_body, indent + 2);
        if (!s.if_else.else_body.empty()) {
            os << ind(indent) << "else\n";
            dump_stmts(os, s.if_else.else_body, indent + 2);
        }
        break;

    case MyIR::Statement::Case:
        os << ind(indent) << "case (" << pretty_name(s.case_stmt.select) << ")\n";
        for (auto &item : s.case_stmt.items) {
            if (item.values.empty())
                os << ind(indent + 2) << "default:\n";
            else {
                os << ind(indent + 2);
                for (size_t i = 0; i < item.values.size(); i++) {
                    if (i) os << ", ";
                    os << item.values[i];
                }
                os << ":\n";
            }
            dump_stmts(os, item.body, indent + 4);
        }
        os << ind(indent) << "endcase\n";
        break;

    case MyIR::Statement::Block:
        os << ind(indent) << "begin\n";
        dump_stmts(os, s.block_body, indent + 2);
        os << ind(indent) << "end\n";
        break;
    }
}

static void dump_module(std::ostream &os, const MyIR::Module &m)
{
    os << "module " << pretty_name(m.name) << "\n";

    for (auto &p : m.ports)
        os << "  port " << portdir_str(p.dir) << " "
           << pretty_name(p.name) << " : " << p.width << " bit"
           << (p.is_signed ? " signed" : "") << "\n";

    for (auto &wkv : m.wires) {
        const MyIR::Port &w = wkv.second;
        if (w.dir != MyIR::PortDir::Internal) continue;
        os << "  wire   " << pretty_name(w.name) << " : "
           << w.width << " bit"
           << (w.is_signed ? " signed" : "") << "\n";
    }

    os << "\n";

    for (size_t i = 0; i < m.exprs.size(); i++) {
        const MyIR::Expr &e = m.exprs[i];
        os << "  expr " << MyIR::optype_to_string(e.op);
        os << "  $" << i << " = ";
        for (size_t j = 0; j < e.operands.size(); j++) {
            if (j) os << ", ";
            os << pretty_name(e.operands[j]);
        }
        os << "  -> " << pretty_name(e.result_wire)
           << " : " << e.result_width << " bit\n";
    }

    for (auto &c : m.connects)
        os << "  connect " << pretty_name(c.lhs) << " = "
           << pretty_name(c.rhs) << "\n";

    for (size_t pi = 0; pi < m.proc_blocks.size(); pi++) {
        auto &pb = m.proc_blocks[pi];
        os << "\n  always @(";
        for (size_t si = 0; si < pb.sensitivity.size(); si++) {
            if (si) os << " or ";
            auto &s = pb.sensitivity[si];
            os << MyIR::sens_type_str(s.type);
            if (!s.signal.empty())
                os << " " << pretty_name(s.signal);
        }
        os << ")  // proc_block " << pi << "\n";
        dump_stmts(os, pb.body, 4);
        os << "  end\n";
    }

    os << "end\n\n";
}

struct MyIRDumpPass : public Pass {
    MyIRDumpPass() : Pass("iris_dump",
        "output human-readable MyIR text format") {}

    void help() override
    {
        log("\n");
        log("    iris_dump [filename]\n");
        log("\n");
        log("Print the current MyIR design in a human-readable text format.\n");
        log("If a filename is given, the output is written to that file;\n");
        log("otherwise it is printed to the Yosys log (stdout).\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing iris_dump.\n");

        std::string filename;
        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx][0] == '-')
                break;
            filename = args[argidx];
            argidx++;
            break;
        }
        extra_args(args, argidx, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design to dump.\n");
            return;
        }

        if (filename.empty()) {
            std::ostringstream oss;
            oss << "# MyIR text format\n";
            for (auto &mkv : myir->modules)
                dump_module(oss, mkv.second);
            log("%s", oss.str().c_str());
        } else {
            std::ofstream ofs(filename);
            if (!ofs.is_open())
                log_error("Cannot open '%s' for writing.\n", filename.c_str());
            ofs << "# MyIR text format\n";
            for (auto &mkv : myir->modules)
                dump_module(ofs, mkv.second);
            log("  Written to '%s'.\n", filename.c_str());
        }
    }
} MyIRDumpPass;

PRIVATE_NAMESPACE_END
