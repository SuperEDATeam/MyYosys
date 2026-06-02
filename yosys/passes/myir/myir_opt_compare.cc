#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

// True if `name` is a constant equal to zero (any width).
static bool is_zero_const(const std::string &name)
{
    return MyIR::is_constant_name(name) && MyIR::const_to_uint(name) == 0;
}

// Lookup signedness for a wire in module `m`.  Constants are treated as
// unsigned; unknown wires return false (conservative).
static bool wire_is_signed(const MyIR::Module &m, const std::string &name)
{
    if (MyIR::is_constant_name(name))
        return false;
    auto it = m.wires.find(name);
    if (it == m.wires.end())
        return false;
    return it->second.is_signed;
}

struct CompareResult {
    bool matched = false;
    int score = 0;
    std::string assign_rhs;
    std::string rule_name;
};

static std::string bool_const(bool v)
{
    return v ? std::string("1'1") : std::string("1'0");
}

// True if both operands refer to the same wire or constant (literal
// string equality) -- safe because is_constant_name names are unique
// and wire names are unique.
static bool same_operand(const std::string &a, const std::string &b)
{
    return !a.empty() && a == b;
}

} // namespace

struct MyIROptComparePass : public Pass {
    MyIROptComparePass() : Pass("myir_opt_compare",
        "structural comparator simplification on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_compare\n");
        log("\n");
        log("Simplify comparator expressions that myir_opt_expr's all-constant\n");
        log("folding cannot reach.  Covers:\n");
        log("\n");
        log("  eq_self    x == x  ->  1     ne_self    x != x  ->  0\n");
        log("  lt_self    x <  x  ->  0     gt_self    x >  x  ->  0\n");
        log("  le_self    x <= x  ->  1     ge_self    x >= x  ->  1\n");
        log("\n");
        log("And, on UNSIGNED operands only (wire.is_signed == false):\n");
        log("\n");
        log("  unsigned_lt_zero   unsigned_x  <  0  ->  0\n");
        log("  unsigned_ge_zero   unsigned_x  >= 0  ->  1\n");
        log("\n");
        log("All rules turn the comparator into a single 1-bit connect, so they\n");
        log("are universally profitable.  This pass nicely feeds myir_opt_muxtree:\n");
        log("after `(a==a) ? c : d' collapses to `1'b1 ? c : d', muxtree finishes\n");
        log("the job and drops the mux entirely.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_compare.\n");
        extra_args(args, 1, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded.\n");
            return;
        }

        int total = 0;
        std::map<std::string, int> rule_hits;

        for (auto &mkv : myir->modules)
        {
            MyIR::Module &m = mkv.second;
            bool changed = true;

            while (changed) {
                changed = false;
                std::vector<MyIR::Expr> new_exprs;
                new_exprs.reserve(m.exprs.size());

                for (auto &e : m.exprs)
                {
                    CompareResult res;

                    if (e.operands.size() == 2) {
                        const std::string &a = e.operands[0];
                        const std::string &b = e.operands[1];
                        const bool eq = same_operand(a, b);

                        // x op x family -- only valid when both refer to
                        // the same expression operand (literal name match).
                        if (eq) {
                            switch (e.op) {
                            case MyIR::OpType::Eq:
                                res.matched = true;
                                res.assign_rhs = bool_const(true);
                                res.rule_name = "eq_self";
                                break;
                            case MyIR::OpType::Ne:
                                res.matched = true;
                                res.assign_rhs = bool_const(false);
                                res.rule_name = "ne_self";
                                break;
                            case MyIR::OpType::Lt:
                                res.matched = true;
                                res.assign_rhs = bool_const(false);
                                res.rule_name = "lt_self";
                                break;
                            case MyIR::OpType::Gt:
                                res.matched = true;
                                res.assign_rhs = bool_const(false);
                                res.rule_name = "gt_self";
                                break;
                            case MyIR::OpType::Le:
                                res.matched = true;
                                res.assign_rhs = bool_const(true);
                                res.rule_name = "le_self";
                                break;
                            case MyIR::OpType::Ge:
                                res.matched = true;
                                res.assign_rhs = bool_const(true);
                                res.rule_name = "ge_self";
                                break;
                            default:
                                break;
                            }
                        }

                        // Unsigned x < 0  -> 0,  unsigned x >= 0  -> 1.
                        if (!res.matched && is_zero_const(b) &&
                            !wire_is_signed(m, a)) {
                            if (e.op == MyIR::OpType::Lt) {
                                res.matched = true;
                                res.assign_rhs = bool_const(false);
                                res.rule_name = "unsigned_lt_zero";
                            } else if (e.op == MyIR::OpType::Ge) {
                                res.matched = true;
                                res.assign_rhs = bool_const(true);
                                res.rule_name = "unsigned_ge_zero";
                            }
                        }
                    }

                    if (!res.matched) {
                        new_exprs.push_back(e);
                        continue;
                    }

                    res.score = MyIR::transform_benefit_score(1, 0, 0, 0, 1);
                    if (res.score <= 0) {
                        new_exprs.push_back(e);
                        continue;
                    }

                    log("  [opt_compare] %s on %s in module '%s' -> %s\n",
                        res.rule_name.c_str(),
                        e.result_wire.c_str(),
                        m.name.c_str(),
                        res.assign_rhs.c_str());
                    rule_hits[res.rule_name]++;
                    total++;

                    MyIR::Assign a;
                    a.lhs = e.result_wire;
                    a.rhs = res.assign_rhs;
                    m.connects.push_back(a);
                    changed = true;
                }

                m.exprs.swap(new_exprs);
            }
        }

        log("  myir_opt_compare summary:\n");
        log("    total simplifications : %d\n", total);
        if (!rule_hits.empty()) {
            log("    by rule:\n");
            for (const auto &kv : rule_hits)
                log("      %-22s : %d\n", kv.first.c_str(), kv.second);
        }
    }
} MyIROptComparePass;

PRIVATE_NAMESPACE_END
