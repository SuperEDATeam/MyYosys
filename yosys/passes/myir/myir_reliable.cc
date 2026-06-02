#include "kernel/yosys.h"
#include <fstream>
#include <vector>
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// ====================================================================
// myir_reliable -- per-module equivalence check between two paths:
//   gold = original AST -> RTLIL  (read_verilog -> proc -> opt_clean)
//   gate = AST -> MyIR -> RTLIL  (myir_ast_import -> [opts] -> myir_export
//                                  -> proc -> opt_clean)
//
// Pipeline stages (each wrapped in its own try/catch so we can locate
// the exact failure point of every module):
//
//   1. Gold       : copy + proc + memory + opt_clean
//   2. AstImport  : copy + myir_ast_import
//   3. Opts       : (optional) myir_opts / myir_opt_<name>
//   4. Export     : myir_export [-strict]
//   5. GatePost   : proc + memory + opt_clean
//   6. EquivMake  : equiv_make
//   7. EquivCheck : equiv_simple + equiv_induct + equiv_status -assert
//
// Result reporting:
//   - tabular summary by stage at end of execute()
//   - optional CSV via -report <file>
// ====================================================================

enum class FailStage {
    None,
    Gold,
    AstImport,
    Opts,
    Export,
    GatePost,
    EquivMake,
    EquivCheck,
};

static const char *stage_name(FailStage s)
{
    switch (s) {
    case FailStage::None:        return "ok";
    case FailStage::Gold:        return "gold";
    case FailStage::AstImport:   return "ast_import";
    case FailStage::Opts:        return "opts";
    case FailStage::Export:      return "export";
    case FailStage::GatePost:    return "gate_post";
    case FailStage::EquivMake:   return "equiv_make";
    case FailStage::EquivCheck:  return "equiv_check";
    }
    return "?";
}

struct ModuleResult {
    std::string module;
    bool        ok = false;
    FailStage   stage = FailStage::None;
    std::string note;
};

static void try_delete_saved_design(RTLIL::Design* design, const std::string& name)
{
    try {
        Pass::call(design, "design -delete " + name);
    }
    catch (...) {
    }
}

// Build the gate-path optimization command sequence depending on
// -stage / -with-opts / -strict-export flags.
static std::vector<std::string> build_opt_chain(
    bool with_opts, const std::string &stage)
{
    std::vector<std::string> v;
    if (!stage.empty()) {
        if (stage == "ast") {
            // pure translate-and-back, no optimization
        } else if (stage == "const")   { v.push_back("myir_opt_const"); }
        else if (stage == "muxtree")   { v.push_back("myir_opt_muxtree"); }
        else if (stage == "expr")      { v.push_back("myir_opt_expr"); }
        else if (stage == "rewrite")   { v.push_back("myir_rewrite"); }
        else if (stage == "share")     { v.push_back("myir_opt_share"); }
        else if (stage == "clean")     { v.push_back("myir_opt_clean"); }
        else if (stage == "opts")      { v.push_back("myir_opts -fixpoint"); }
        // unknown stage names are silently treated as "ast"
        return v;
    }
    if (with_opts)
        v.push_back("myir_opts -fixpoint");
    return v;
}

static ModuleResult verify_module_with_myir(
    RTLIL::Design* design,
    RTLIL::IdString mod_name,
    const std::string& src_design_name,
    bool with_opts,
    bool strict_export,
    bool recursive,
    const std::string &stage_filter)
{
    ModuleResult r;
    r.module = log_id(mod_name);

    const std::string mod = mod_name.str();
    const std::string gold_design = "__myir_rel_gold_design";
    const std::string gate_design = "__myir_rel_gate_design";
    const std::string gold_mod = "\\__myir_rel_gold";
    const std::string gate_mod = "\\__myir_rel_gate";
    const std::string equiv_mod = "\\__myir_rel_equiv";

    try_delete_saved_design(design, gold_design);
    try_delete_saved_design(design, gate_design);

    // -copy-from supports either a single module or '*' for everything;
    // EPFL flat designs use the fast single-module path.
    const std::string copy_arg = recursive ? "*" : mod;
    const std::string copy_from =
        "design -copy-from " + src_design_name + " " + copy_arg;

    // ---- Stage 1: Gold ----
    try {
        Pass::call(design, "design -reset");
        Pass::call(design, copy_from);
        Pass::call(design, "proc");
        Pass::call(design, "memory");
        Pass::call(design, "opt_clean");
        Pass::call(design, "design -save " + gold_design);
    } catch (...) {
        r.stage = FailStage::Gold;
        r.note  = "gold path build failed";
        goto cleanup;
    }

    // ---- Stage 2: AstImport ----
    try {
        Pass::call(design, "design -reset");
        Pass::call(design, copy_from);
        Pass::call(design, "myir_ast_import");
    } catch (...) {
        r.stage = FailStage::AstImport;
        r.note  = "myir_ast_import failed";
        goto cleanup;
    }

    // ---- Stage 3: Opts ----
    try {
        for (const auto &cmd : build_opt_chain(with_opts, stage_filter))
            Pass::call(design, cmd);
    } catch (...) {
        r.stage = FailStage::Opts;
        r.note  = "opt chain failed";
        goto cleanup;
    }

    // ---- Stage 4: Export ----
    try {
        Pass::call(design, strict_export ? "myir_export -strict"
                                          : "myir_export");
    } catch (...) {
        r.stage = FailStage::Export;
        r.note  = "myir_export failed";
        goto cleanup;
    }

    // ---- Stage 5: GatePost ----
    try {
        Pass::call(design, "proc");
        Pass::call(design, "memory");
        Pass::call(design, "opt_clean");
        Pass::call(design, "design -save " + gate_design);
    } catch (...) {
        r.stage = FailStage::GatePost;
        r.note  = "gate post-processing failed";
        goto cleanup;
    }

    // ---- Stage 6: EquivMake ----
    try {
        Pass::call(design, "design -reset");
        Pass::call(design, "design -copy-from " + gold_design + " -as " + gold_mod + " " + mod);
        Pass::call(design, "design -copy-from " + gate_design + " -as " + gate_mod + " " + mod);
        Pass::call(design, "equiv_make " + gold_mod + " " + gate_mod + " " + equiv_mod);
    } catch (...) {
        r.stage = FailStage::EquivMake;
        r.note  = "equiv_make failed";
        goto cleanup;
    }

    // ---- Stage 7: EquivCheck ----
    try {
        Pass::call(design, "equiv_simple " + equiv_mod);
        Pass::call(design, "equiv_induct " + equiv_mod);
        Pass::call(design, "equiv_status -assert " + equiv_mod);
    } catch (...) {
        r.stage = FailStage::EquivCheck;
        r.note  = "equiv check found non-equivalent points";
        goto cleanup;
    }

    r.ok = true;
    r.stage = FailStage::None;

cleanup:
    try_delete_saved_design(design, gold_design);
    try_delete_saved_design(design, gate_design);
    return r;
}

struct MyIRReliablePass : public Pass {
    MyIRReliablePass() : Pass("myir_reliable",
        "check whether AST -> MyIR -> RTLIL stays equivalent module-by-module") {
    }

    void help() override
    {
        log("\n");
        log("    myir_reliable [options]\n");
        log("\n");
        log("Run a per-module reliability check on the current selection.\n");
        log("For each selected module, this command compares:\n");
        log("\n");
        log("    gold = original AST -> proc/memory/opt_clean -> RTLIL\n");
        log("    gate = original AST -> myir_ast_import -> [optional opts]\n");
        log("                          -> myir_export -> proc/memory/opt_clean\n");
        log("                          -> RTLIL\n");
        log("\n");
        log("using:\n");
        log("    equiv_make\n");
        log("    equiv_simple\n");
        log("    equiv_induct\n");
        log("    equiv_status -assert\n");
        log("\n");
        log("Each module is run through 7 internally-tracked stages\n");
        log("(gold, ast_import, opts, export, gate_post, equiv_make,\n");
        log(" equiv_check) so a failure can be attributed to a single\n");
        log("stage in the summary.\n");
        log("\n");
        log("Options:\n");
        log("\n");
        log("    -with-opts\n");
        log("        Insert 'myir_opts -fixpoint' between myir_ast_import\n");
        log("        and myir_export, so the check covers the full\n");
        log("        optimization pipeline.  Default: no optimization\n");
        log("        is run (only translate-and-back is verified).\n");
        log("\n");
        log("    -stage <name>\n");
        log("        Verify a single optimization pass in isolation,\n");
        log("        instead of -with-opts.  Recognized names:\n");
        log("            ast      (no opt, equivalent to default)\n");
        log("            const | muxtree | expr | rewrite\n");
        log("            share | clean\n");
        log("            opts     (alias of -with-opts)\n");
        log("\n");
        log("    -strict-export\n");
        log("        Forward -strict to myir_export so any silent drop\n");
        log("        (proc_blocks, Unknown OpType, no-celltype) is\n");
        log("        promoted to a hard error and shows up as a Export\n");
        log("        stage failure in the report.\n");
        log("\n");
        log("    -recursive\n");
        log("        Copy all modules (design -copy-from src *) for each\n");
        log("        verification round, instead of just the module under\n");
        log("        test.  Needed for hierarchical designs with sub-\n");
        log("        modules; EPFL flat designs do not need this.\n");
        log("\n");
        log("    -report <file>\n");
        log("        Write a per-module CSV report:\n");
        log("            module,ok,fail_stage,note\n");
        log("\n");
        log("End-of-pass summary breaks unreliable modules down by stage,\n");
        log("so you can tell apart 'export dropped something' from\n");
        log("'optimization produced non-equivalent RTLIL'.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design* design) override
    {
        log_header(design, "Executing myir_reliable.\n");

        bool         with_opts     = false;
        bool         strict_export = false;
        bool         recursive     = false;
        std::string  stage_filter;
        std::string  report_path;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            const std::string &a = args[argidx];
            if (a == "-with-opts")     { with_opts = true; continue; }
            if (a == "-strict-export") { strict_export = true; continue; }
            if (a == "-recursive")     { recursive = true; continue; }
            if (a == "-stage" && argidx + 1 < args.size()) {
                stage_filter = args[++argidx];
                continue;
            }
            if (a == "-report" && argidx + 1 < args.size()) {
                report_path = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

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

        std::vector<ModuleResult> results;
        results.reserve(modules.size());

        for (auto mod_name : modules) {
            log("Checking module %s ...\n", log_id(mod_name));
            ModuleResult r = verify_module_with_myir(
                design, mod_name, src_design,
                with_opts, strict_export, recursive, stage_filter);
            results.push_back(r);
        }

        Pass::call(design, "design -reset");
        Pass::call(design, "design -load " + src_design);
        try_delete_saved_design(design, src_design);

        // ---- Summary ---------------------------------------------------
        int reliable = 0, unreliable = 0;
        int cnt_gold = 0, cnt_ast = 0, cnt_opts = 0, cnt_exp = 0;
        int cnt_gpost = 0, cnt_emk = 0, cnt_eck = 0;
        for (auto &r : results) {
            if (r.ok) { reliable++; continue; }
            unreliable++;
            switch (r.stage) {
            case FailStage::Gold:        cnt_gold++;  break;
            case FailStage::AstImport:   cnt_ast++;   break;
            case FailStage::Opts:        cnt_opts++;  break;
            case FailStage::Export:      cnt_exp++;   break;
            case FailStage::GatePost:    cnt_gpost++; break;
            case FailStage::EquivMake:   cnt_emk++;   break;
            case FailStage::EquivCheck:  cnt_eck++;   break;
            default: break;
            }
        }

        log("\n  myir_reliable summary:\n");
        log("    total modules          : %d\n", (int)results.size());
        log("    reliable               : %d\n", reliable);
        log("    unreliable             : %d\n", unreliable);
        if (unreliable > 0) {
            log("    breakdown by stage:\n");
            if (cnt_gold)  log("      gold          : %d\n", cnt_gold);
            if (cnt_ast)   log("      ast_import    : %d\n", cnt_ast);
            if (cnt_opts)  log("      opts          : %d\n", cnt_opts);
            if (cnt_exp)   log("      export        : %d\n", cnt_exp);
            if (cnt_gpost) log("      gate_post     : %d\n", cnt_gpost);
            if (cnt_emk)   log("      equiv_make    : %d\n", cnt_emk);
            if (cnt_eck)   log("      equiv_check   : %d\n", cnt_eck);
        }

        log("\n  Reliable modules (%d):\n", reliable);
        for (auto &r : results)
            if (r.ok) log("    %s\n", r.module.c_str());

        if (unreliable > 0) {
            log("\n  Unreliable modules (%d):\n", unreliable);
            for (auto &r : results)
                if (!r.ok)
                    log("    %-30s  [%s]  %s\n",
                        r.module.c_str(),
                        stage_name(r.stage),
                        r.note.c_str());
        }

        if (unreliable == 0)
            log("\n  MyIR is reliable for all selected modules.\n");
        else
            log("\n  MyIR is still growing -- see breakdown above.\n");

        // ---- Optional CSV report --------------------------------------
        if (!report_path.empty()) {
            std::ofstream f(report_path);
            if (!f) {
                log_warning("myir_reliable: cannot open report file '%s' "
                            "for writing.\n", report_path.c_str());
            } else {
                f << "module,ok,fail_stage,note\n";
                for (auto &r : results) {
                    f << r.module << ','
                      << (r.ok ? "1" : "0") << ','
                      << stage_name(r.stage) << ','
                      << '"' << r.note << "\"\n";
                }
                log("  CSV report written to: %s\n", report_path.c_str());
            }
        }
    }
} MyIRReliablePass;

PRIVATE_NAMESPACE_END
