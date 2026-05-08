#include "kernel/yosys.h"
#include "passes/myir/myir.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

static bool all_const(const std::vector<std::string> &ops)
{
    for (auto &o : ops)
        if (!MyIR::is_constant_name(o))
            return false;
    return !ops.empty();
}

static std::string eval_expr(MyIR::OpType op,
                             const std::vector<std::string> &operands,
                             int result_width)
{
    uint64_t a = MyIR::const_to_uint(operands[0]);
    uint64_t b = operands.size() >= 2 ? MyIR::const_to_uint(operands[1]) : 0;
    uint64_t mask = result_width >= 64 ? ~0ULL
                                       : ((1ULL << result_width) - 1);
    uint64_t r = 0;

    switch (op) {
    case MyIR::OpType::And:          r = a & b;              break;
    case MyIR::OpType::Or:           r = a | b;              break;
    case MyIR::OpType::Xor:          r = a ^ b;              break;
    case MyIR::OpType::Xnor:         r = (~(a ^ b)) & mask; break;
    case MyIR::OpType::Not:          r = (~a) & mask;        break;
    case MyIR::OpType::Eq:           r = (a == b) ? 1 : 0;  break;
    case MyIR::OpType::Ne:           r = (a != b) ? 1 : 0;  break;
    case MyIR::OpType::Add:          r = (a + b) & mask;     break;
    case MyIR::OpType::Sub:          r = (a - b) & mask;     break;
    case MyIR::OpType::Mul:          r = (a * b) & mask;     break;
    case MyIR::OpType::Buf:
    case MyIR::OpType::Pos:          r = a & mask;           break;
    case MyIR::OpType::Neg:          r = ((~a) + 1) & mask;  break;
    case MyIR::OpType::Logic_And:    r = (a && b) ? 1 : 0;  break;
    case MyIR::OpType::Logic_Or:     r = (a || b) ? 1 : 0;  break;
    case MyIR::OpType::Logic_Not:    r = (!a) ? 1 : 0;      break;
    case MyIR::OpType::Reduce_Or:    r = (a != 0) ? 1 : 0;  break;
    case MyIR::OpType::Reduce_Bool:  r = (a != 0) ? 1 : 0;  break;
    case MyIR::OpType::Mux: {
        uint64_t s = operands.size() >= 3 ? MyIR::const_to_uint(operands[2]) : 0;
        r = (s & 1) ? b : a;
        break;
    }
    default:
        return "";
    }

    return MyIR::uint_to_const(r, result_width);
}

static bool is_zero_const(const std::string &name, int width)
{
    return MyIR::is_constant_name(name) &&
           MyIR::const_width(name) == width &&
           MyIR::const_to_uint(name) == 0;
}

static bool is_one_const(const std::string &name, int width)
{
    return MyIR::is_constant_name(name) &&
           MyIR::const_width(name) == width &&
           MyIR::const_to_uint(name) == 1;
}

static std::string make_allones_const(int width)
{
    if (width <= 0)
        width = 1;
    return std::to_string(width) + "'" + std::string(width, '1');
}

static bool is_allones_const(const std::string &name, int width)
{
    return MyIR::is_constant_name(name) &&
           MyIR::const_width(name) == width &&
           name == make_allones_const(width);
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

struct SimplifyResult {
    bool matched = false;
    int score = 0;
    bool remove_expr = false;
    bool add_assign = false;
    bool rewrite_expr = false;
    std::string new_rhs;
    MyIR::Expr rewritten_expr;
    std::string rule_name;
};

static SimplifyResult simplify_expr(const MyIR::Expr &e)
{
    SimplifyResult best;
    auto consider_assign = [&](const std::string &rhs, const std::string &rule) {
        SimplifyResult r;
        r.matched = true;
        r.remove_expr = true;
        r.add_assign = true;
        r.new_rhs = rhs;
        r.rule_name = rule;
        r.score = MyIR::transform_benefit_score(
            1, e.op == MyIR::OpType::Mux ? 1 : 0, 0, 0, 1);
        if (r.score > best.score)
            best = r;
    };
    auto consider_rewrite = [&](const MyIR::Expr &rewritten, const std::string &rule) {
        SimplifyResult r;
        r.matched = true;
        r.rewrite_expr = true;
        r.rewritten_expr = rewritten;
        r.rule_name = rule;
        r.score = MyIR::transform_benefit_score(
            1, e.op == MyIR::OpType::Mux ? 1 : 0, 1, 0, 0);
        if (r.score > best.score)
            best = r;
    };

    if (all_const(e.operands)) {
        std::string cval = eval_expr(e.op, e.operands, e.result_width);
        if (!cval.empty())
            consider_assign(cval, "const_fold");
    }

    if (e.op == MyIR::OpType::Buf || e.op == MyIR::OpType::Pos) {
        if (e.operands.size() == 1)
            consider_assign(e.operands[0], "unary_buffer");
        return best;
    }

    if (e.op == MyIR::OpType::Not || e.op == MyIR::OpType::Logic_Not) {
        if (e.operands.size() == 1 && MyIR::is_constant_name(e.operands[0])) {
            std::string cval = eval_expr(e.op, e.operands, e.result_width);
            if (!cval.empty())
                consider_assign(cval, "const_not");
        }
        return best;
    }

    if (e.operands.size() < 2)
        return best;

    const std::string &a = e.operands[0];
    const std::string &b = e.operands[1];
    int width = e.result_width;

    switch (e.op)
    {
    case MyIR::OpType::And:
        if (is_zero_const(a, width) || is_zero_const(b, width))
            consider_assign(MyIR::uint_to_const(0, width), "and_zero");
        if (is_allones_const(a, width))
            consider_assign(b, "and_allones");
        if (is_allones_const(b, width))
            consider_assign(a, "and_allones");
        break;

    case MyIR::OpType::Or:
        if (is_allones_const(a, width) || is_allones_const(b, width))
            consider_assign(make_allones_const(width), "or_allones");
        if (is_zero_const(a, width))
            consider_assign(b, "or_zero");
        if (is_zero_const(b, width))
            consider_assign(a, "or_zero");
        break;

    case MyIR::OpType::Xor:
        if (is_zero_const(a, width))
            consider_assign(b, "xor_zero");
        if (is_zero_const(b, width))
            consider_assign(a, "xor_zero");
        if (is_allones_const(a, width)) {
            MyIR::Expr rewritten = e;
            rewritten.op = MyIR::OpType::Not;
            rewritten.operands = {b};
            consider_rewrite(rewritten, "xor_allones");
        }
        if (is_allones_const(b, width)) {
            MyIR::Expr rewritten = e;
            rewritten.op = MyIR::OpType::Not;
            rewritten.operands = {a};
            consider_rewrite(rewritten, "xor_allones");
        }
        break;

    case MyIR::OpType::Xnor:
        if (is_allones_const(a, width))
            consider_assign(b, "xnor_allones");
        if (is_allones_const(b, width))
            consider_assign(a, "xnor_allones");
        if (is_zero_const(a, width)) {
            MyIR::Expr rewritten = e;
            rewritten.op = MyIR::OpType::Not;
            rewritten.operands = {b};
            consider_rewrite(rewritten, "xnor_zero");
        }
        if (is_zero_const(b, width)) {
            MyIR::Expr rewritten = e;
            rewritten.op = MyIR::OpType::Not;
            rewritten.operands = {a};
            consider_rewrite(rewritten, "xnor_zero");
        }
        break;

    case MyIR::OpType::Add:
    case MyIR::OpType::Sub:
        if (is_zero_const(b, width))
            consider_assign(a, e.op == MyIR::OpType::Add ? "add_zero" : "sub_zero");
        if (e.op == MyIR::OpType::Add && is_zero_const(a, width))
            consider_assign(b, "add_zero");
        break;

    case MyIR::OpType::Mul:
        if (is_zero_const(a, width) || is_zero_const(b, width))
            consider_assign(MyIR::uint_to_const(0, width), "mul_zero");
        if (is_one_const(a, width))
            consider_assign(b, "mul_one");
        if (is_one_const(b, width))
            consider_assign(a, "mul_one");
        break;

    case MyIR::OpType::Logic_And:
        if ((MyIR::is_constant_name(a) && MyIR::const_to_uint(a) == 0) ||
            (MyIR::is_constant_name(b) && MyIR::const_to_uint(b) == 0))
            consider_assign("1'0", "logic_and_zero");
        if (is_one_const(a, 1) && MyIR::is_constant_name(b))
            consider_assign(MyIR::const_to_uint(b) ? "1'1" : "1'0", "logic_and_one");
        if (is_one_const(b, 1) && MyIR::is_constant_name(a))
            consider_assign(MyIR::const_to_uint(a) ? "1'1" : "1'0", "logic_and_one");
        break;

    case MyIR::OpType::Logic_Or:
        if ((MyIR::is_constant_name(a) && MyIR::const_to_uint(a) != 0) ||
            (MyIR::is_constant_name(b) && MyIR::const_to_uint(b) != 0))
            consider_assign("1'1", "logic_or_one");
        if (is_zero_const(a, 1) && MyIR::is_constant_name(b))
            consider_assign(MyIR::const_to_uint(b) ? "1'1" : "1'0", "logic_or_zero");
        if (is_zero_const(b, 1) && MyIR::is_constant_name(a))
            consider_assign(MyIR::const_to_uint(a) ? "1'1" : "1'0", "logic_or_zero");
        break;

    case MyIR::OpType::Mux:
        if (e.operands.size() == 3) {
            const std::string &s = e.operands[2];
            if (a == b)
                consider_assign(a, "mux_same_inputs");
            if (MyIR::is_constant_name(s))
                consider_assign(MyIR::const_to_uint(s) ? b : a, "mux_const_sel");
            if (width == 1 && is_zero_const(a, width) && is_allones_const(b, width))
                consider_assign(s, "mux_bool");
            if (width == 1 && is_allones_const(a, width) && is_zero_const(b, width)) {
                MyIR::Expr rewritten = e;
                rewritten.op = MyIR::OpType::Not;
                rewritten.operands = {s};
                consider_rewrite(rewritten, "mux_invert");
            }
        }
        break;

    default:
        break;
    }

    return best;
}

} // namespace

struct MyIROptExprPass : public Pass {
    MyIROptExprPass() : Pass("myir_opt_expr",
        "perform const folding and simple expression rewriting on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_expr\n");
        log("\n");
        log("Perform expression-level simplification on MyIR.\n");
        log("This pass handles:\n");
        log("  - full constant folding\n");
        log("  - partial constant folding (e.g. x & 0, x | 0)\n");
        log("  - gate degeneration to wires / inverters\n");
        log("  - simple mux simplification\n");
        log("Run myir_opt_const before this pass if you want constants from\n");
        log("existing assigns to be propagated into expressions first.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_expr.\n");
        extra_args(args, 1, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded.\n");
            return;
        }

        int total = 0;

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
                    SimplifyResult simp = simplify_expr(e);
                    if (!simp.matched || simp.score <= 0) {
                        new_exprs.push_back(e);
                        continue;
                    }

                    log("  [opt_expr] %s on %s (score=%d)\n",
                        simp.rule_name.c_str(),
                        e.result_wire.c_str(),
                        simp.score);

                    if (simp.remove_expr) {
                        if (simp.add_assign && e.result_wire != simp.new_rhs) {
                            replace_all_uses(m, e.result_wire, simp.new_rhs);
                            MyIR::Assign a;
                            a.lhs = e.result_wire;
                            a.rhs = simp.new_rhs;
                            m.connects.push_back(a);
                        }
                        changed = true;
                        total++;
                        continue;
                    }

                    if (simp.rewrite_expr) {
                        new_exprs.push_back(simp.rewritten_expr);
                        changed = true;
                        total++;
                        continue;
                    }

                    new_exprs.push_back(e);
                }

                m.exprs.swap(new_exprs);
            }
        }

        log("  Applied %d expression simplification(s).\n", total);
    }
} MyIROptExprPass;

PRIVATE_NAMESPACE_END
