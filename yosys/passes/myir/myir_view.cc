#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct MyIRViewPass : public Pass {
    MyIRViewPass() : Pass("iris_view",
        "open an already-generated image or DOT file") {}

    void help() override
    {
        log("\n");
        log("    iris_view <file> [-viewer <cmd>]\n");
        log("\n");
        log("Open an already-generated image or DOT file with the OS\n");
        log("default associated viewer (or a custom command via -viewer).\n");
        log("\n");
        log("Unlike 'iris_show', this command does NOT regenerate the\n");
        log("file; it just opens whatever path you provide.  Typical use:\n");
        log("\n");
        log("    yosys> iris_show                # generate files only\n");
        log("    yosys> iris_view iris_show.png  # view later, no regen\n");
        log("\n");
        log("    -viewer <cmd>    open the file with <cmd> \"<file>\"\n");
        log("                     instead of the OS default viewer\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing iris_view.\n");

        std::string file;
        std::string viewer;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-viewer" && argidx + 1 < args.size()) {
                viewer = args[++argidx];
                continue;
            }
            if (!args[argidx].empty() && args[argidx][0] != '-'
                && file.empty()) {
                file = args[argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        if (file.empty()) {
            log_error("iris_view: missing <file> argument.\n"
                      "  Usage: iris_view <file> [-viewer <cmd>]\n"
                      "  Run 'iris_show' first to generate a .dot / image\n"
                      "  file, then pass its path to 'iris_view'.\n");
            return;
        }

        {
            std::ifstream probe(file.c_str());
            if (!probe.good())
                log_error("iris_view: file '%s' not found or unreadable.\n",
                          file.c_str());
        }

        std::string cmd;
        if (!viewer.empty())
            cmd = viewer + " \"" + file + "\"";
        else
            cmd = MyIR::make_open_cmd(file);

        log("  Exec: %s\n", cmd.c_str());
        int rc = run_command(cmd);
        if (rc != 0)
            log_warning("iris_view: viewer command returned %d.\n", rc);
    }
} MyIRViewPass;

PRIVATE_NAMESPACE_END
