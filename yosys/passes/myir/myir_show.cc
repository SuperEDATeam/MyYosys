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

static std::string emit_stmt_dot(std::ostream &os, const MyIR::Statement &s,
                                  const std::string &prefix, int &sid);

static void emit_stmts_dot(std::ostream &os,
                            const std::vector<MyIR::Statement> &stmts,
                            const std::string &prefix, int &sid,
                            const std::string &from_id = "")
{
    std::string prev = from_id;
    for (auto &s : stmts) {
        std::string nid = emit_stmt_dot(os, s, prefix, sid);
        if (!prev.empty())
            os << "    \"" << prev << "\" -> \"" << nid << "\";\n";
        prev = nid;
    }
}

static std::string emit_stmt_dot(std::ostream &os, const MyIR::Statement &s,
                                  const std::string &prefix, int &sid)
{
    std::string nid = prefix + "_s" + std::to_string(sid++);

    switch (s.kind) {
    case MyIR::Statement::Assign:
        os << "    \"" << nid << "\" [label=\""
           << sanitize(s.proc_assign.lhs) << " "
           << (s.proc_assign.is_blocking ? "=" : "<=") << " "
           << sanitize(s.proc_assign.rhs)
           << "\", shape=box, style=filled, fillcolor=\"#e0f0e0\"];\n";
        break;

    case MyIR::Statement::If:
        os << "    \"" << nid << "\" [label=\"if ("
           << sanitize(s.if_else.condition)
           << ")\", shape=diamond, style=filled, fillcolor=\"#ffe0e0\"];\n";
        {
            std::string then_id = nid + "_T";
            std::string else_id = nid + "_F";
            int sub = 0;
            os << "    \"" << then_id << "\" [label=\"then\", shape=plaintext];\n";
            os << "    \"" << nid << "\" -> \"" << then_id << "\";\n";
            emit_stmts_dot(os, s.if_else.then_body, then_id, sub, then_id);
            if (!s.if_else.else_body.empty()) {
                sub = 0;
                os << "    \"" << else_id << "\" [label=\"else\", shape=plaintext];\n";
                os << "    \"" << nid << "\" -> \"" << else_id << "\";\n";
                emit_stmts_dot(os, s.if_else.else_body, else_id, sub, else_id);
            }
        }
        break;

    case MyIR::Statement::Case:
        os << "    \"" << nid << "\" [label=\"case ("
           << sanitize(s.case_stmt.select)
           << ")\", shape=trapezium, style=filled, fillcolor=\"#e0e0ff\"];\n";
        for (size_t ci = 0; ci < s.case_stmt.items.size(); ci++) {
            std::string br_id = nid + "_br" + std::to_string(ci);
            std::string lbl;
            if (s.case_stmt.items[ci].values.empty())
                lbl = "default";
            else {
                for (size_t vi = 0; vi < s.case_stmt.items[ci].values.size(); vi++) {
                    if (vi) lbl += ",";
                    lbl += s.case_stmt.items[ci].values[vi];
                }
            }
            os << "    \"" << br_id << "\" [label=\"" << lbl
               << "\", shape=plaintext];\n";
            os << "    \"" << nid << "\" -> \"" << br_id << "\";\n";
            int sub = 0;
            emit_stmts_dot(os, s.case_stmt.items[ci].body, br_id, sub, br_id);
        }
        break;

    case MyIR::Statement::Block:
        os << "    \"" << nid << "\" [label=\"block\", shape=plaintext];\n";
        emit_stmts_dot(os, s.block_body, nid, sid, nid);
        break;
    }

    return nid;
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

    // procedural blocks
    for (size_t pi = 0; pi < m.proc_blocks.size(); pi++) {
        auto &pb = m.proc_blocks[pi];
        std::string sens_label;
        for (size_t si = 0; si < pb.sensitivity.size(); si++) {
            if (si) sens_label += " or ";
            sens_label += MyIR::sens_type_str(pb.sensitivity[si].type);
            if (!pb.sensitivity[si].signal.empty())
                sens_label += " " + label_name(pb.sensitivity[si].signal);
        }
        os << "\n  subgraph cluster_proc_" << pi << " {\n";
        os << "    label=\"always @(" << sanitize(sens_label) << ")\";\n";
        os << "    style=dashed;\n";
        int sid = 0;
        emit_stmts_dot(os, pb.body, "p" + std::to_string(pi), sid);
        os << "  }\n";
    }

    os << "}\n";
}

struct MyIRShowPass : public Pass {
    MyIRShowPass() : Pass("myir_show",
        "generate DOT graph from MyIR design") {}

    void help() override
    {
        log("\n");
        log("    myir_show [-o <dotfile>] [-fmt <format>] [-view]\n");
        log("             [-viewer <cmd>] [-no-image]\n");
        log("\n");
        log("Generate a Graphviz DOT file and image for the current MyIR\n");
        log("design.\n");
        log("\n");
        log("    -o <file>        write DOT to <file> (default: myir_show.dot)\n");
        log("    -fmt <format>    image format: png|pdf|svg (default: png)\n");
        log("    -view            open the image with the OS default\n");
        log("                     associated viewer after generation\n");
        log("    -viewer <cmd>    open the image with a custom command\n");
        log("                     (takes precedence over -view)\n");
        log("    -no-image        only write DOT; do not invoke Graphviz\n");
        log("\n");
        log("To open an already-generated file without regenerating it,\n");
        log("use the 'myir_view' command.\n");
        log("\n");
        log("Graphviz 'dot' is located in the following order:\n");
        log("  1. <exe_dir>/tools/graphviz/bin/dot(.exe) and up to 3\n");
        log("     parent directories (covers repo root for VS builds)\n");
        log("  2. MYIR_DOT environment variable\n");
        log("  3. system PATH\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_show.\n");

        std::string dot_file = "myir_show.dot";
        std::string fmt      = "png";
        std::string viewer;
        bool flag_view     = false;
        bool flag_no_image = false;

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
            if (args[argidx] == "-view") {
                flag_view = true;
                continue;
            }
            if (args[argidx] == "-no-image") {
                flag_no_image = true;
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

        // -- 1) write .dot ---------------------------------------------------
        {
            std::ofstream ofs(dot_file);
            if (!ofs.is_open())
                log_error("Cannot open '%s' for writing.\n", dot_file.c_str());
            for (auto &mkv : myir->modules)
                generate_dot(ofs, mkv.second);
        }
        log("  DOT written to '%s'.\n", dot_file.c_str());

        if (flag_no_image) {
            log("  -no-image specified; skipping Graphviz invocation.\n");
            return;
        }

        // -- 2) derive output image path ------------------------------------
        std::string img_file = dot_file;
        {
            size_t dpos = img_file.rfind('.');
            if (dpos != std::string::npos)
                img_file = img_file.substr(0, dpos);
            img_file += "." + fmt;
        }

        // -- 3) locate a usable 'dot' ---------------------------------------
        std::string dot_exe = MyIR::find_dot_executable();
        if (dot_exe.empty()) {
            log_error("myir_show: Graphviz 'dot' command not found.\n"
                      "  Option 1: Install Graphviz and add its 'bin' folder\n"
                      "            to PATH.  https://graphviz.org/download/\n"
                      "  Option 2: Extract a Graphviz portable build into\n"
                      "            <repo>/tools/graphviz/  so that\n"
                      "            <repo>/tools/graphviz/bin/dot(.exe)\n"
                      "            exists. (Also accepted: the same layout\n"
                      "            under the executable's own directory,\n"
                      "            or up to 3 parent directories above it.)\n"
                      "  Option 3: Set the MYIR_DOT environment variable to\n"
                      "            the full path of dot(.exe).\n"
                      "  (Use 'myir_show -no-image' to skip image generation\n"
                      "   and produce only the .dot file.)\n");
            return; // log_error normally does not return, but keep for safety
        }
        log("  Using dot: %s\n", dot_exe.c_str());

        // -- 4) run dot -----------------------------------------------------
        std::string dot_cmd = dot_exe + " -T" + fmt + " -o \"" + img_file
                             + "\" \"" + dot_file + "\"";
        log("  Running: %s\n", dot_cmd.c_str());
        int rc = run_command(dot_cmd);
        if (rc != 0) {
            log_warning("Graphviz 'dot' returned %d. "
                        "The image may not have been generated.\n", rc);
            return;
        }
        log("  Image written to '%s'.\n", img_file.c_str());

        // -- 5) optional viewer --------------------------------------------
        if (!viewer.empty()) {
            std::string view_cmd = viewer + " \"" + img_file + "\"";
            log("  Exec: %s\n", view_cmd.c_str());
            run_command(view_cmd);
        } else if (flag_view) {
            std::string view_cmd = MyIR::make_open_cmd(img_file);
            log("  Exec: %s\n", view_cmd.c_str());
            run_command(view_cmd);
        } else {
            log("  Use -view to open now, or run "
                "'myir_view %s' later.\n", img_file.c_str());
        }
    }
} MyIRShowPass;

PRIVATE_NAMESPACE_END
