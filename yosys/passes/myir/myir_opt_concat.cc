#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <map>
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

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

// Width of an operand: constants self-describe, wires look up the
// per-module wires table.  Returns 0 on unknown (caller should skip).
static int operand_width(const MyIR::Module &m, const std::string &name)
{
    if (MyIR::is_constant_name(name))
        return MyIR::const_width(name);
    auto it = m.wires.find(name);
    if (it == m.wires.end())
        return 0;
    return it->second.width;
}

static bool all_const(const std::vector<std::string> &ops)
{
    if (ops.empty()) return false;
    for (auto &o : ops)
        if (!MyIR::is_constant_name(o))
            return false;
    return true;
}

// Fold a constant Concat into one big constant.  Concat semantics:
// operands[0] is the LEAST significant chunk (the MyIR/AST import stores
// `{a, b, c}` least-significant-first, so operands[0] occupies the low
// bits).  Each successive chunk is therefore shifted further left.
static bool fold_all_constants(const MyIR::Expr &e, std::string &out)
{
    uint64_t value = 0;
    int total_width = 0;
    for (auto &op : e.operands) {
        int w = MyIR::const_width(op);
        uint64_t v = MyIR::const_to_uint(op);
        if (total_width + w > 64)
            return false;        // outside our uint_to_const range
        value |= (v & (w >= 64 ? ~0ULL : ((1ULL << w) - 1))) << total_width;
        total_width += w;
    }
    if (total_width == 0)
        return false;
    out = MyIR::uint_to_const(value, total_width);
    return true;
}

// Try to flatten nested Concats:
//   Concat(a, Concat(b, c), d)  ->  Concat(a, b, c, d)
// when the inner Concat is only used once.  Returns true and fills
// `new_operands` on success.
static bool flatten_nested(const MyIR::Expr &e,
                           const MyIR::Module &m,
                           const std::unordered_map<std::string, int> &producer,
                           const std::unordered_map<std::string, int> &uses,
                           std::vector<std::string> &new_operands)
{
    bool any_change = false;
    new_operands.clear();
    new_operands.reserve(e.operands.size());

    for (const auto &op : e.operands) {
        auto p_it = producer.find(op);
        if (p_it == producer.end()) {
            new_operands.push_back(op);
            continue;
        }
        const MyIR::Expr &inner = m.exprs[p_it->second];
        if (inner.op != MyIR::OpType::Concat) {
            new_operands.push_back(op);
            continue;
        }
        auto u_it = uses.find(inner.result_wire);
        if (u_it == uses.end() || u_it->second != 1) {
            new_operands.push_back(op);
            continue;
        }
        // Splice the inner Concat's operands into our list.
        for (const auto &inner_op : inner.operands)
            new_operands.push_back(inner_op);
        any_change = true;
    }
    return any_change;
}

// Merge consecutive constant operands into a single bigger constant:
//   Concat(a, 2'b00, 2'b11, b)  ->  Concat(a, 4'b0011, b)
// Returns true and fills `new_operands` on success.
static bool merge_adjacent_consts(const std::vector<std::string> &in,
                                  std::vector<std::string> &out)
{
    out.clear();
    out.reserve(in.size());
    bool any_change = false;

    size_t i = 0;
    while (i < in.size()) {
        if (!MyIR::is_constant_name(in[i])) {
            out.push_back(in[i]);
            i++;
            continue;
        }

        size_t j = i + 1;
        while (j < in.size() && MyIR::is_constant_name(in[j]))
            j++;

        if (j == i + 1) {
            // Single constant, no merge possible.
            out.push_back(in[i]);
            i = j;
            continue;
        }

        // Merge [i, j) into one constant.  Concat operands are
        // least-significant first, so in[i] occupies the low bits and each
        // later chunk is shifted further left.
        uint64_t value = 0;
        int total_width = 0;
        bool overflow = false;
        for (size_t t = i; t < j; t++) {
            int w = MyIR::const_width(in[t]);
            uint64_t v = MyIR::const_to_uint(in[t]);
            if (total_width + w > 64) { overflow = true; break; }
            value |= (v & (w >= 64 ? ~0ULL : ((1ULL << w) - 1)))
                  << total_width;
            total_width += w;
        }
        if (overflow) {
            for (size_t t = i; t < j; t++)
                out.push_back(in[t]);
        } else {
            out.push_back(MyIR::uint_to_const(value, total_width));
            any_change = true;
        }
        i = j;
    }
    return any_change;
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

struct MyIROptConcatPass : public Pass {
    MyIROptConcatPass() : Pass("myir_opt_concat",
        "fold and flatten MyIR Concat expressions") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_concat\n");
        log("\n");
        log("Optimise `Concat` expressions, which no other MyIR pass touches:\n");
        log("\n");
        log("  all_const   Concat of constants only       ->  single constant\n");
        log("  single_op   Concat with one operand        ->  Buf (alias)\n");
        log("  adj_const   Adjacent constants in a Concat ->  merged constant\n");
        log("  flatten     Concat(.., Concat(..) [single-use], ..)\n");
        log("              ->  Concat(.., inner operands, ..)\n");
        log("\n");
        log("Concat operands are stored least-significant first: operands[0]\n");
        log("occupies the low bits.  Folding therefore shifts each successive\n");
        log("piece further left by the combined width of the pieces below it.\n");
        log("\n");
        log("All four rules are unconditionally profitable (cell count drops or\n");
        log("stays the same while later passes get a cleaner Concat to look at).\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_concat.\n");
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
                    if (e.op != MyIR::OpType::Concat) {
                        new_exprs.push_back(e);
                        continue;
                    }

                    // Rule 1: all-constant -> single constant assign.
                    if (all_const(e.operands)) {
                        std::string folded;
                        if (fold_all_constants(e, folded)) {
                            log("  [opt_concat] all_const on %s in module '%s' -> %s\n",
                                e.result_wire.c_str(),
                                m.name.c_str(),
                                folded.c_str());
                            rule_hits["all_const"]++;
                            total++;

                            replace_all_uses(m, e.result_wire, folded);
                            MyIR::Assign a;
                            a.lhs = e.result_wire;
                            a.rhs = folded;
                            m.connects.push_back(a);
                            changed = true;
                            continue;
                        }
                    }

                    // Rule 2: single operand -> Buf alias.
                    if (e.operands.size() == 1) {
                        // Sanity: width still has to match.
                        int op_w = operand_width(m, e.operands[0]);
                        if (op_w != 0 && op_w == e.result_width) {
                            log("  [opt_concat] single_op on %s in module '%s'\n",
                                e.result_wire.c_str(), m.name.c_str());
                            rule_hits["single_op"]++;
                            total++;

                            MyIR::Expr rewritten = e;
                            rewritten.op = MyIR::OpType::Buf;
                            // operands already a single entry.
                            new_exprs.push_back(rewritten);
                            changed = true;
                            continue;
                        }
                    }

                    // Rule 3+4: flatten nested + merge adjacent constants.
                    std::vector<std::string> flattened;
                    bool did_flatten = flatten_nested(e, m, producer, uses, flattened);

                    std::vector<std::string> merged;
                    bool did_merge = merge_adjacent_consts(
                        did_flatten ? flattened : e.operands, merged);

                    if (did_flatten || did_merge) {
                        if (did_flatten) {
                            log("  [opt_concat] flatten on %s in module '%s' "
                                "(%d -> %d operands)\n",
                                e.result_wire.c_str(), m.name.c_str(),
                                (int)e.operands.size(),
                                did_merge ? (int)merged.size()
                                          : (int)flattened.size());
                            rule_hits["flatten"]++;
                        }
                        if (did_merge) {
                            log("  [opt_concat] adj_const on %s in module '%s'\n",
                                e.result_wire.c_str(), m.name.c_str());
                            rule_hits["adj_const"]++;
                        }
                        total += (did_flatten ? 1 : 0) + (did_merge ? 1 : 0);

                        MyIR::Expr rewritten = e;
                        rewritten.operands = did_merge ? merged : flattened;
                        new_exprs.push_back(rewritten);
                        changed = true;
                        continue;
                    }

                    new_exprs.push_back(e);
                }

                m.exprs.swap(new_exprs);
            }
        }

        log("  myir_opt_concat summary:\n");
        log("    total rewrites : %d\n", total);
        if (!rule_hits.empty()) {
            log("    by rule:\n");
            for (const auto &kv : rule_hits)
                log("      %-12s : %d\n", kv.first.c_str(), kv.second);
        }
    }
} MyIROptConcatPass;

PRIVATE_NAMESPACE_END
