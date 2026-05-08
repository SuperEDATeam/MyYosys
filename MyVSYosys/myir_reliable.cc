#include "kernel/yosys.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static void try_delete_saved_design(RTLIL::Design* design, const std::string& name)
{
    try {
        Pass::call(design, "design -delete " + name);
    }
    catch (...) {
    }
}

static bool verify_module_with_myir(RTLIL::Design* design, RTLIL::IdString mod_name,
    const std::string& src_design_name)
{
    const std::string mod = mod_name.str();

    const std::string gold_design = "__myir_rel_gold_design";
    const std::string gate_design = "__myir_rel_gate_design";
    const std::string gold_mod = "\\__myir_rel_gold";
    const std::string gate_mod = "\\__myir_rel_gate";
    const std::string equiv_mod = "\\__myir_rel_equiv";

    try_delete_saved_design(design, gold_design);
    try_delete_saved_design(design, gate_design);

    try {
        Pass::call(design, "design -reset");
        Pass::call(design, "design -copy-from " + src_design_name + " " + mod);
        Pass::call(design, "proc");
        Pass::call(design, "memory");
        Pass::call(design, "opt_clean");
        Pass::call(design, "design -save " + gold_design);

        Pass::call(design, "design -reset");
        Pass::call(design, "design -copy-from " + src_design_name + " " + mod);
        Pass::call(design, "myir_ast_import");
        Pass::call(design, "myir_export");
        Pass::call(design, "proc");
        Pass::call(design, "memory");
        Pass::call(design, "opt_clean");
        Pass::call(design, "design -save " + gate_design);

        Pass::call(design, "design -reset");
        Pass::call(design, "design -copy-from " + gold_design + " -as " + gold_mod + " " + mod);
        Pass::call(design, "design -copy-from " + gate_design + " -as " + gate_mod + " " + mod);
        Pass::call(design, "equiv_make " + gold_mod + " " + gate_mod + " " + equiv_mod);
        Pass::call(design, "equiv_simple " + equiv_mod);
        Pass::call(design, "equiv_induct " + equiv_mod);
        Pass::call(design, "equiv_status -assert " + equiv_mod);

        try_delete_saved_design(design, gold_design);
        try_delete_saved_design(design, gate_design);
        return true;
    }
    catch (...) {
        try_delete_saved_design(design, gold_design);
        try_delete_saved_design(design, gate_design);
        return false;
    }
}

struct MyIRReliablePass : public Pass {
    MyIRReliablePass() : Pass("myir_reliable",
        "check whether AST -> MyIR -> RTLIL stays equivalent module-by-module") {
    }

    void help() override
    {
        log("\n");
        log("    myir_reliable\n");
        log("\n");
        log("Run a per-module reliability check on the current selection.\n");
        log("For each selected module, this command compares:\n");
        log("\n");
        log("    original Verilog/AST -> RTLIL\n");
        log("    original Verilog/AST -> MyIR -> RTLIL\n");
        log("\n");
        log("using:\n");
        log("    equiv_make\n");
        log("    equiv_simple\n");
        log("    equiv_induct\n");
        log("    equiv_status -assert\n");
        log("\n");
        log("Modules that fail any step are reported as unreliable.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design* design) override
    {
        log_header(design, "Executing myir_reliable.\n");
        extra_args(args, 1, design);

        std::vector<RTLIL::IdString> modules;
        for (auto module : design->selected_modules())
            modules.push_back(module->name);

        if (modules.empty()) {
            log_warning("myir_reliable: no selected modules.\n");
            return;
        }

        const std::string src_design = "__myir_rel_source";
        try_delete_saved_design(design, src_design);
        Pass::call(design, "design -save " + src_design);

        std::vector<std::string> reliable_modules;
        std::vector<std::string> unreliable_modules;

        for (auto mod_name : modules) {
            log("Checking module %s ...\n", log_id(mod_name));
            bool ok = verify_module_with_myir(design, mod_name, src_design);
            if (ok)
                reliable_modules.push_back(log_id(mod_name));
            else
                unreliable_modules.push_back(log_id(mod_name));
        }

        Pass::call(design, "design -reset");
        Pass::call(design, "design -load " + src_design);
        try_delete_saved_design(design, src_design);

        log("\nReliable modules (%d):\n", (int)reliable_modules.size());
        for (auto& name : reliable_modules)
            log("  %s\n", name.c_str());

        log("\nUnreliable modules (%d):\n", (int)unreliable_modules.size());
        for (auto& name : unreliable_modules)
            log("  %s\n", name.c_str());

        if (unreliable_modules.empty())
            log("MyIR is always reliable.\n");
        else
            log("Sorry, MyIR is still growing.\n");
    }
} MyIRReliablePass;

PRIVATE_NAMESPACE_END