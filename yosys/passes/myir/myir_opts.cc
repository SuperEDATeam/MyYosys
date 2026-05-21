#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <cstddef>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

struct OptsOptions {
    bool fixpoint   = false;
    bool stats      = false;
    int  k          = -1;             // -1 = leave default
    bool no_const   = false;
    bool no_expr    = false;
    bool no_muxtree = false;
    bool no_rewrite = false;
    bool no_share   = false;
    bool no_clean   = false;
};

// Compact summary of a MyIR design used to detect when an optimisation
// round produced no further changes.
static size_t design_fingerprint(MyIR::Design *d)
{
    if (!d)
        return 0;
    size_t h = 0;
    for (const auto &kv : d->modules) {
        const auto &m = kv.second;
        h = h * 131 + m.exprs.size();
        h = h * 131 + m.connects.size();
        h = h * 131 + m.wires.size();
    }
    return h;
}

static void run_one_round(RTLIL::Design *design, const OptsOptions &o)
{
    std::string k_arg = o.k > 0 ? (" -k " + std::to_string(o.k)) : "";
    std::string rewrite_extra = o.stats ? " -stats" : "";

    if (!o.no_const)    Pass::call(design, "myir_opt_const");
    if (!o.no_expr)     Pass::call(design, "myir_opt_expr");
    if (!o.no_const)    Pass::call(design, "myir_opt_const");
    if (!o.no_muxtree)  Pass::call(design, "myir_opt_muxtree");
    if (!o.no_expr)     Pass::call(design, "myir_opt_expr");
    if (!o.no_rewrite)  Pass::call(design, "myir_rewrite" + k_arg + rewrite_extra);
    if (!o.no_share)    Pass::call(design, "myir_opt_share");
    if (!o.no_clean)    Pass::call(design, "myir_opt_clean");
}

} // namespace

struct MyIROptsPass : public Pass {
    MyIROptsPass() : Pass("myir_opts",
        "run the full MyIR optimisation pipeline in one go") {}

    void help() override
    {
        log("\n");
        log("    myir_opts [-once | -fixpoint] [-k N] [-stats]\n");
        log("              [-noconst] [-noexpr] [-nomuxtree] [-norewrite]\n");
        log("              [-noshare] [-noclean]\n");
        log("\n");
        log("Run the standard MyIR optimisation pipeline:\n");
        log("  myir_opt_const -> myir_opt_expr -> myir_opt_const\n");
        log("  -> myir_opt_muxtree -> myir_opt_expr -> myir_rewrite\n");
        log("  -> myir_opt_share -> myir_opt_clean\n");
        log("\n");
        log("By default the pipeline is executed once.  Use -fixpoint to\n");
        log("iterate until no further changes are observed (or 10 rounds,\n");
        log("whichever comes first).\n");
        log("\n");
        log("Options:\n");
        log("    -once       Run the pipeline exactly once (default).\n");
        log("    -fixpoint   Iterate the pipeline up to 10 rounds, stopping\n");
        log("                early when a round produces no IR change.\n");
        log("    -k N        Forwarded to 'myir_rewrite -k N'.\n");
        log("    -stats      Forwarded to 'myir_rewrite -stats'.\n");
        log("    -noconst    Skip myir_opt_const.\n");
        log("    -noexpr     Skip myir_opt_expr.\n");
        log("    -nomuxtree  Skip myir_opt_muxtree.\n");
        log("    -norewrite  Skip myir_rewrite.\n");
        log("    -noshare    Skip myir_opt_share.\n");
        log("    -noclean    Skip myir_opt_clean.\n");
        log("\n");
        log("This is the recommended entry point for everyday use.  The\n");
        log("individual passes are still useful when debugging a specific\n");
        log("optimisation or comparing two pipelines.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opts.\n");

        OptsOptions o;
        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++)
        {
            const std::string &a = args[argidx];
            if (a == "-once")       { o.fixpoint = false; continue; }
            if (a == "-fixpoint")   { o.fixpoint = true;  continue; }
            if (a == "-stats")      { o.stats    = true;  continue; }
            if (a == "-noconst")    { o.no_const   = true; continue; }
            if (a == "-noexpr")     { o.no_expr    = true; continue; }
            if (a == "-nomuxtree")  { o.no_muxtree = true; continue; }
            if (a == "-norewrite")  { o.no_rewrite = true; continue; }
            if (a == "-noshare")    { o.no_share   = true; continue; }
            if (a == "-noclean")    { o.no_clean   = true; continue; }
            if (a == "-k" && argidx + 1 < args.size()) {
                o.k = atoi(args[++argidx].c_str());
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        if (o.k != -1 && o.k < 2)
            log_error("myir_opts: -k must be >= 2.\n");

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded; nothing to optimise.  "
                        "Did you forget 'myir_import' or 'myir_ast_import'?\n");
            return;
        }

        const int max_rounds = 10;
        size_t prev = design_fingerprint(myir);
        int round = 1;

        while (true) {
            log_header(design, "myir_opts: round %d.\n", round);
            run_one_round(design, o);

            myir = MyIR::get_design();        // pipeline may have replaced it
            size_t cur = design_fingerprint(myir);

            if (!o.fixpoint) {
                log("myir_opts: completed one round (use -fixpoint to iterate).\n");
                break;
            }

            if (cur == prev) {
                log("myir_opts: converged after %d round(s).\n", round);
                break;
            }

            if (round >= max_rounds) {
                log("myir_opts: stopped after %d round(s) (max_rounds reached).\n",
                    round);
                break;
            }

            prev = cur;
            round++;
        }
    }
} MyIROptsPass;

PRIVATE_NAMESPACE_END
