#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <fstream>
#include <sstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static std::string sanitize(const std::string &s)
{
    std::string r;
    for (char c : s) {
        if (c == '\\' || c == '"')
            r += '_';
        else if (c == '$')
            r += '_';
        else
            r += c;
    }
    return r;
}

static std::string label_name(const std::string &s)
{
    if (!s.empty() && (s[0] == '\\' || s[0] == '$'))
        return s.substr(1);
    return s;
}

static void generate_dot(std::ostream &os, const MyIR::Module &m)
{
    os << "digraph \"" << sanitize(m.name) << "\" {\n";
    os << "  rankdir=LR;\n";
    os << "  label=\"" << label_name(m.name) << "\";\n";
    os << "  node [fontname=\"Helvetica\", fontsize=10];\n";
    os << "  edge [fontsize=8];\n\n";

    // wire/port nodes
    for (auto &wkv : m.wires) {
        const MyIR::Port &p = wkv.second;
        std::string nid = "w_" + sanitize(p.name);
        std::string lbl = label_name(p.name) + "\\n"
                        + std::to_string(p.width) + " bit";

        if (p.dir == MyIR::PortDir::Input) {
            os << "  \"" << nid << "\" [label=\"" << lbl
               << "\", shape=octagon, style=filled, "
                  "fillcolor=lightblue];\n";
        } else if (p.dir == MyIR::PortDir::Output) {
            os << "  \"" << nid << "\" [label=\"" << lbl
               << "\", shape=octagon, style=filled, "
                  "fillcolor=lightyellow];\n";
        } else if (p.dir == MyIR::PortDir::InOut) {
            os << "  \"" << nid << "\" [label=\"" << lbl
               << "\", shape=octagon, style=filled, "
                  "fillcolor=lightgreen];\n";
        } else {
            os << "  \"" << nid << "\" [label=\"" << lbl
               << "\", shape=ellipse];\n";
        }
    }

    // constant nodes (collected from operands)
    std::set<std::string> const_nodes;
    for (auto &e : m.exprs)
        for (auto &op : e.operands)
            if (MyIR::is_constant_name(op))
                const_nodes.insert(op);
    for (auto &c : const_nodes) {
        std::string nid = "c_" + sanitize(c);
        os << "  \"" << nid << "\" [label=\"" << c
           << "\", shape=diamond, style=filled, fillcolor=lightgray];\n";
    }

    os << "\n";

    // expression nodes + edges
    for (size_t i = 0; i < m.exprs.size(); i++) {
        const MyIR::Expr &e = m.exprs[i];
        std::string eid = "e_" + std::to_string(i);
        std::string elbl = MyIR::optype_to_string(e.op);

        os << "  \"" << eid << "\" [label=\"" << elbl
           << "\", shape=box, style=\"rounded,filled\", "
              "fillcolor=\"#f0e0d0\"];\n";

        for (auto &op : e.operands) {
            std::string src = MyIR::is_constant_name(op)
                                  ? "c_" + sanitize(op)
                                  : "w_" + sanitize(op);
            os << "  \"" << src << "\" -> \"" << eid << "\";\n";
        }

        std::string dst = "w_" + sanitize(e.result_wire);
        os << "  \"" << eid << "\" -> \"" << dst << "\";\n";
    }

    // assign edges
    for (auto &a : m.connects) {
        std::string src = MyIR::is_constant_name(a.rhs)
                              ? "c_" + sanitize(a.rhs)
                              : "w_" + sanitize(a.rhs);
        std::string dst = "w_" + sanitize(a.lhs);
        os << "  \"" << src << "\" -> \"" << dst
           << "\" [style=dashed, color=gray];\n";
    }

    os << "}\n";
}

struct MyIRShowPass : public Pass {
    MyIRShowPass() : Pass("myir_show",
        "generate DOT graph from MyIR design") {}

    void help() override
    {
        log("\n");
        log("    myir_show [-o <dotfile>] [-fmt <format>] [-viewer <cmd>]\n");
        log("\n");
        log("Generate a Graphviz DOT file visualising the MyIR design.\n");
        log("\n");
        log("    -o <file>        write DOT to <file> (default: myir_show.dot)\n");
        log("    -fmt <format>    output image format, e.g. png, pdf (default: png)\n");
        log("    -viewer <cmd>    open result with <cmd> (e.g. 'start' on Windows)\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_show.\n");

        std::string dot_file = "myir_show.dot";
        std::string fmt      = "png";
        std::string viewer;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-o" && argidx + 1 < args.size()) {
                dot_file = args[++argidx];
                continue;
            }
            if (args[argidx] == "-fmt" && argidx + 1 < args.size()) {
                fmt = args[++argidx];
                continue;
            }
            if (args[argidx] == "-viewer" && argidx + 1 < args.size()) {
                viewer = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design to show.\n");
            return;
        }

        {
            std::ofstream ofs(dot_file);
            if (!ofs.is_open())
                log_error("Cannot open '%s' for writing.\n", dot_file.c_str());
            for (auto &mkv : myir->modules)
                generate_dot(ofs, mkv.second);
        }
        log("  DOT written to '%s'.\n", dot_file.c_str());

        // try to convert to image using Graphviz
        std::string img_file = dot_file;
        {
            size_t dpos = img_file.rfind('.');
            if (dpos != std::string::npos)
                img_file = img_file.substr(0, dpos);
            img_file += "." + fmt;
        }

        std::string dot_cmd = "dot -T" + fmt + " -o \"" + img_file
                             + "\" \"" + dot_file + "\"";
        log("  Running: %s\n", dot_cmd.c_str());
        int rc = run_command(dot_cmd);
        if (rc != 0)
            log_warning("Graphviz 'dot' returned %d. "
                        "Is Graphviz installed and in PATH?\n", rc);
        else
            log("  Image written to '%s'.\n", img_file.c_str());

        if (!viewer.empty() && rc == 0) {
            std::string view_cmd = viewer + " \"" + img_file + "\"";
            run_command(view_cmd);
        }
    }
} MyIRShowPass;

PRIVATE_NAMESPACE_END
