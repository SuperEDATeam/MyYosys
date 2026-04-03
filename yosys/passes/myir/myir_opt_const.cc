#include "kernel/yosys.h"
#include "passes/myir/myir.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

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
    case MyIR::OpType::And:          r = a & b;                break;
    case MyIR::OpType::Or:           r = a | b;                break;
    case MyIR::OpType::Xor:          r = a ^ b;                break;
    case MyIR::OpType::Not:          r = (~a) & mask;          break;
    case MyIR::OpType::Eq:           r = (a == b) ? 1 : 0;    break;
    case MyIR::OpType::Ne:           r = (a != b) ? 1 : 0;    break;
    case MyIR::OpType::Lt:           r = (a <  b) ? 1 : 0;    break;
    case MyIR::OpType::Le:           r = (a <= b) ? 1 : 0;    break;
    case MyIR::OpType::Gt:           r = (a >  b) ? 1 : 0;    break;
    case MyIR::OpType::Ge:           r = (a >= b) ? 1 : 0;    break;
    case MyIR::OpType::Add:          r = (a + b) & mask;       break;
    case MyIR::OpType::Sub:          r = (a - b) & mask;       break;
    case MyIR::OpType::Mul:          r = (a * b) & mask;       break;
    case MyIR::OpType::Buf:
    case MyIR::OpType::Pos:          r = a & mask;             break;
    case MyIR::OpType::Neg:          r = ((~a) + 1) & mask;    break;
    case MyIR::OpType::Logic_And:    r = (a && b) ? 1 : 0;    break;
    case MyIR::OpType::Logic_Or:     r = (a || b) ? 1 : 0;    break;
    case MyIR::OpType::Logic_Not:    r = (!a) ? 1 : 0;        break;
    case MyIR::OpType::Reduce_And: {
        int aw = MyIR::const_width(operands[0]);
        uint64_t amask = aw >= 64 ? ~0ULL : ((1ULL << aw) - 1);
        r = (a == amask) ? 1 : 0;
        break;
    }
    case MyIR::OpType::Reduce_Or:    r = (a != 0) ? 1 : 0;    break;
    case MyIR::OpType::Reduce_Xor: {
        uint64_t t = a;
        r = 0;
        while (t) { r ^= (t & 1); t >>= 1; }
        break;
    }
    case MyIR::OpType::Shl:          r = (a << b) & mask;      break;
    case MyIR::OpType::Shr:          r = (a >> b) & mask;      break;
    case MyIR::OpType::Sshl:         r = (a << b) & mask;      break;
    case MyIR::OpType::Sshr:         r = (a >> b) & mask;      break;
    case MyIR::OpType::Mux: {
        uint64_t s = operands.size() >= 3
                         ? MyIR::const_to_uint(operands[2]) : 0;
        r = (s & 1) ? b : a;
        break;
    }
    default:
        return "";
    }

    return MyIR::uint_to_const(r, result_width);
}

struct MyIROptConstPass : public Pass {
    MyIROptConstPass() : Pass("myir_opt_const",
        "constant propagation / folding on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_const\n");
        log("\n");
        log("Evaluate expressions whose operands are all constants and\n");
        log("replace them with the computed constant value.  Also propagate\n");
        log("constants through Buf / Pos / direct assigns.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_const.\n");
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

                // propagate constants through assigns
                for (auto &c : m.connects) {
                    if (MyIR::is_constant_name(c.rhs)) {
                        for (auto &e : m.exprs)
                            for (auto &op : e.operands)
                                if (op == c.lhs) {
                                    op = c.rhs;
                                    changed = true;
                                }
                    }
                }

                // fold constant expressions
                std::vector<MyIR::Expr> new_exprs;
                for (auto &e : m.exprs) {
                    if (all_const(e.operands)) {
                        std::string cval = eval_expr(e.op, e.operands,
                                                     e.result_width);
                        if (!cval.empty()) {
                            log("  [opt_const] folded %s -> %s = %s\n",
                                MyIR::optype_to_string(e.op),
                                e.result_wire.c_str(), cval.c_str());

                            // replace all uses of result_wire with the constant
                            for (auto &e2 : m.exprs)
                                for (auto &op : e2.operands)
                                    if (op == e.result_wire) op = cval;
                            for (auto &a : m.connects)
                                if (a.rhs == e.result_wire) a.rhs = cval;

                            // keep as assign so the wire retains its value
                            MyIR::Assign ca;
                            ca.lhs = e.result_wire;
                            ca.rhs = cval;
                            m.connects.push_back(ca);

                            ++total;
                            changed = true;
                            continue;
                        }
                    }
                    new_exprs.push_back(e);
                }
                m.exprs = new_exprs;
            }
        }

        log("  Folded %d constant expression(s).\n", total);
    }
} MyIROptConstPass;

PRIVATE_NAMESPACE_END
