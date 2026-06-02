#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <map>
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

// 32-bit constant operand for shift amounts.  Matches the width Verilog
// integer literals default to and keeps RTLIL export happy.
static const int SHIFT_AMOUNT_WIDTH = 32;

static std::unordered_map<std::string, int> build_producer_map(const MyIR::Module &m)
{
    std::unordered_map<std::string, int> producer;
    for (int i = 0; i < (int)m.exprs.size(); i++)
        producer[m.exprs[i].result_wire] = i;
    return producer;
}

static std::unordered_map<std::string, int> build_use_count(const MyIR::Module &m)
{
    std::unordered_map<std::string, int> uses;
    for (const auto &e : m.exprs)
        for (const auto &op : e.operands)
            uses[op]++;
    for (const auto &c : m.connects)
        uses[c.rhs]++;
    return uses;
}

// True if `name` is a constant equal to zero (any width).
static bool is_zero_const(const std::string &name)
{
    return MyIR::is_constant_name(name) && MyIR::const_to_uint(name) == 0;
}

// Detect power-of-two constants.  Returns the exponent in *k_out when
// the constant has exactly one set bit.
static bool is_pow2_const(const std::string &name, int &k_out)
{
    if (!MyIR::is_constant_name(name))
        return false;
    uint64_t v = MyIR::const_to_uint(name);
    if (v == 0 || (v & (v - 1)) != 0)
        return false;
    k_out = 0;
    while ((v >>= 1) != 0)
        k_out++;
    return true;
}

struct StrengthResult {
    bool matched = false;
    int score = 0;
    bool replace_with_assign = false;        // remove expr, add connect
    bool rewrite_expr = false;               // replace expr in place
    std::string assign_rhs;                  // for replace_with_assign
    MyIR::Expr rewritten_expr;               // for rewrite_expr
    std::string rule_name;
};

static void try_rule(StrengthResult &best, const StrengthResult &cand)
{
    if (cand.matched && cand.score > best.score)
        best = cand;
}

// Build "x * 2^k -> x << k", commutative on both sides.
static void match_mul_pow2(const MyIR::Expr &e, StrengthResult &best)
{
    if (e.op != MyIR::OpType::Mul || e.operands.size() != 2)
        return;

    int k = -1;
    std::string value_wire;
    if (is_pow2_const(e.operands[1], k))
        value_wire = e.operands[0];
    else if (is_pow2_const(e.operands[0], k))
        value_wire = e.operands[1];
    else
        return;

    // x * 1 is already handled by myir_opt_expr; skip k == 0 to avoid
    // a useless Shl(x, 0) rewrite that does not save anything.
    if (k == 0)
        return;

    StrengthResult r;
    r.matched = true;
    r.rewrite_expr = true;
    r.rule_name = "mul_pow2_to_shl";
    r.rewritten_expr = e;
    r.rewritten_expr.op = MyIR::OpType::Shl;
    r.rewritten_expr.operands = { value_wire,
                                  MyIR::uint_to_const(k, SHIFT_AMOUNT_WIDTH) };
    // Mul -> Shl: -1 cell with a much cheaper one.  Score it as a wash
    // on cell count but heavily favouring the cheaper variant via
    // removed_exprs only (no added_exprs penalty).
    r.score = MyIR::transform_benefit_score(1, 0, 1, 0, 0);
    // Treat as positive even though cell count is unchanged.
    if (r.score <= 0)
        r.score = 1;
    try_rule(best, r);
}

// Add(x, x) -> Shl(x, 1)
static void match_add_self(const MyIR::Expr &e, StrengthResult &best)
{
    if (e.op != MyIR::OpType::Add || e.operands.size() != 2)
        return;
    if (e.operands[0] != e.operands[1])
        return;

    StrengthResult r;
    r.matched = true;
    r.rewrite_expr = true;
    r.rule_name = "add_self_to_shl";
    r.rewritten_expr = e;
    r.rewritten_expr.op = MyIR::OpType::Shl;
    r.rewritten_expr.operands = { e.operands[0],
                                  MyIR::uint_to_const(1, SHIFT_AMOUNT_WIDTH) };
    r.score = MyIR::transform_benefit_score(1, 0, 1, 0, 0);
    if (r.score <= 0)
        r.score = 1;
    try_rule(best, r);
}

// Sub(0, x) -> Neg(x)
static void match_sub_from_zero(const MyIR::Expr &e, StrengthResult &best)
{
    if (e.op != MyIR::OpType::Sub || e.operands.size() != 2)
        return;
    if (!is_zero_const(e.operands[0]))
        return;

    StrengthResult r;
    r.matched = true;
    r.rewrite_expr = true;
    r.rule_name = "sub_from_zero_to_neg";
    r.rewritten_expr = e;
    r.rewritten_expr.op = MyIR::OpType::Neg;
    r.rewritten_expr.operands = { e.operands[1] };
    r.score = MyIR::transform_benefit_score(1, 0, 1, 0, 0);
    if (r.score <= 0)
        r.score = 1;
    try_rule(best, r);
}

// Neg(Neg(x)) -> x  (replace with direct connect)
static void match_double_neg(const MyIR::Expr &e,
                             const MyIR::Module &m,
                             const std::unordered_map<std::string, int> &producer,
                             const std::unordered_map<std::string, int> &uses,
                             StrengthResult &best)
{
    if (e.op != MyIR::OpType::Neg || e.operands.size() != 1)
        return;

    auto it = producer.find(e.operands[0]);
    if (it == producer.end())
        return;
    const MyIR::Expr &inner = m.exprs[it->second];
    if (inner.op != MyIR::OpType::Neg || inner.operands.size() != 1)
        return;
    // Only fold if the inner Neg is exclusively used by this outer Neg;
    // otherwise we would orphan its other consumers' producer.
    auto u_it = uses.find(inner.result_wire);
    if (u_it != uses.end() && u_it->second != 1)
        return;

    StrengthResult r;
    r.matched = true;
    r.replace_with_assign = true;
    r.rule_name = "double_neg";
    r.assign_rhs = inner.operands[0];
    r.score = MyIR::transform_benefit_score(2, 0, 0, 0, 1);
    try_rule(best, r);
}

static void replace_all_uses(MyIR::Module &m,
                             const std::string &old_wire,
                             const std::string &new_wire)
{
    for (auto &e : m.exprs)
        for (auto &op : e.operands)
            if (op == old_wire)
                op = new_wire;
    for (auto &c : m.connects)
        if (c.rhs == old_wire)
            c.rhs = new_wire;
}

} // namespace

struct MyIROptStrengthPass : public Pass {
    MyIROptStrengthPass() : Pass("myir_opt_strength",
        "arithmetic strength reduction on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_strength\n");
        log("\n");
        log("Replace expensive arithmetic operations with cheaper equivalents:\n");
        log("\n");
        log("  mul_pow2_to_shl       x * 2^k   ->  x << k\n");
        log("  add_self_to_shl       x + x     ->  x << 1\n");
        log("  sub_from_zero_to_neg  0 - x     ->  -x   (Neg)\n");
        log("  double_neg            -(-x)     ->  x\n");
        log("\n");
        log("All rules preserve bit-level semantics (the multiplier/adder/sub\n");
        log("operand widths are kept on the resulting shift / Neg cell).  No new\n");
        log("internal wires are introduced.\n");
        log("\n");
        log("This pass is intended to run AFTER myir_opt_const / myir_opt_expr\n");
        log("so that obvious x*1, x*0, 0+x etc. are already resolved.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_strength.\n");
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
                auto producer = build_producer_map(m);
                auto uses = build_use_count(m);

                std::vector<MyIR::Expr> new_exprs;
                new_exprs.reserve(m.exprs.size());

                for (auto &e : m.exprs)
                {
                    StrengthResult best;
                    match_mul_pow2(e, best);
                    match_add_self(e, best);
                    match_sub_from_zero(e, best);
                    match_double_neg(e, m, producer, uses, best);

                    if (!best.matched || best.score <= 0) {
                        new_exprs.push_back(e);
                        continue;
                    }

                    log("  [opt_strength] %s on %s in module '%s' (score=%d)\n",
                        best.rule_name.c_str(),
                        e.result_wire.c_str(),
                        m.name.c_str(),
                        best.score);
                    rule_hits[best.rule_name]++;
                    total++;

                    if (best.replace_with_assign) {
                        if (e.result_wire != best.assign_rhs) {
                            replace_all_uses(m, e.result_wire, best.assign_rhs);
                            MyIR::Assign a;
                            a.lhs = e.result_wire;
                            a.rhs = best.assign_rhs;
                            m.connects.push_back(a);
                        }
                        changed = true;
                        continue;
                    }

                    if (best.rewrite_expr) {
                        new_exprs.push_back(best.rewritten_expr);
                        changed = true;
                        continue;
                    }

                    new_exprs.push_back(e);
                }

                m.exprs.swap(new_exprs);
            }
        }

        log("  myir_opt_strength summary:\n");
        log("    total reductions : %d\n", total);
        if (!rule_hits.empty()) {
            log("    by rule:\n");
            for (const auto &kv : rule_hits)
                log("      %-22s : %d\n", kv.first.c_str(), kv.second);
        }
    }
} MyIROptStrengthPass;

PRIVATE_NAMESPACE_END
