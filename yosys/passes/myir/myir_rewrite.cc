#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static bool is_factorable_parent(MyIR::OpType op)
{
    return op == MyIR::OpType::Or || op == MyIR::OpType::And;
}

static bool is_factorable_child(MyIR::OpType parent, MyIR::OpType child)
{
    if (parent == MyIR::OpType::Or)
        return child == MyIR::OpType::And;
    if (parent == MyIR::OpType::And)
        return child == MyIR::OpType::Or;
    return false;
}

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

static std::string fresh_wire_name(const MyIR::Module &m, int &counter)
{
    while (true) {
        std::string name = "$rewrite$" + std::to_string(counter++);
        if (!m.wires.count(name))
            return name;
    }
}

static bool extract_common_operand(const MyIR::Expr &lhs, const MyIR::Expr &rhs,
                                   std::string &common,
                                   std::string &lhs_other,
                                   std::string &rhs_other)
{
    if (lhs.operands.size() != 2 || rhs.operands.size() != 2)
        return false;

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            if (lhs.operands[i] != rhs.operands[j])
                continue;

            common = lhs.operands[i];
            lhs_other = lhs.operands[1 - i];
            rhs_other = rhs.operands[1 - j];
            return true;
        }
    }

    return false;
}

struct MyIRRewritePass : public Pass {
    MyIRRewritePass() : Pass("myir_rewrite",
        "local algebraic rewriting on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_rewrite\n");
        log("\n");
        log("Run local pattern-based rewriting on MyIR expressions.\n");
        log("Currently implemented rules:\n");
        log("  (a & b) | (a & c)  ->  a & (b | c)\n");
        log("  (a | b) & (a | c)  ->  a | (b & c)\n");
        log("\n");
        log("The pass only rewrites a local 3-node cut when both child\n");
        log("sub-expressions are exclusively used by the root, so the\n");
        log("replacement reduces node count from 3 to 2 as intended.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_rewrite.\n");
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
            int rewrite_counter = 0;
            bool changed = true;

            while (changed) {
                changed = false;

                auto producer = build_producer_map(m);
                auto uses = build_use_count(m);
                std::vector<MyIR::Expr> rewritten_exprs;
                rewritten_exprs.reserve(m.exprs.size());

                for (int idx = 0; idx < (int)m.exprs.size(); idx++)
                {
                    MyIR::Expr root = m.exprs[idx];
                    bool rewritten = false;

                    if (is_factorable_parent(root.op) && root.operands.size() == 2) {
                        auto it_l = producer.find(root.operands[0]);
                        auto it_r = producer.find(root.operands[1]);

                        if (it_l != producer.end() && it_r != producer.end() &&
                            it_l->second != it_r->second) {
                            const MyIR::Expr &lhs = m.exprs[it_l->second];
                            const MyIR::Expr &rhs = m.exprs[it_r->second];

                            if (lhs.result_width == root.result_width &&
                                rhs.result_width == root.result_width &&
                                is_factorable_child(root.op, lhs.op) &&
                                is_factorable_child(root.op, rhs.op) &&
                                uses[lhs.result_wire] == 1 &&
                                uses[rhs.result_wire] == 1) {
                                std::string common, lhs_other, rhs_other;
                                if (extract_common_operand(lhs, rhs, common,
                                                           lhs_other, rhs_other) &&
                                    lhs_other != rhs_other) {
                                    std::string temp = fresh_wire_name(m, rewrite_counter);

                                    MyIR::Port w;
                                    w.name = temp;
                                    w.dir = MyIR::PortDir::Internal;
                                    w.width = root.result_width;
                                    m.wires[temp] = w;

                                    MyIR::Expr inner;
                                    inner.op = root.op;
                                    inner.result_wire = temp;
                                    inner.result_width = root.result_width;
                                    inner.operands.push_back(lhs_other);
                                    inner.operands.push_back(rhs_other);

                                    root.op = lhs.op;
                                    root.operands.clear();
                                    root.operands.push_back(common);
                                    root.operands.push_back(temp);

                                    rewritten_exprs.push_back(inner);
                                    rewritten_exprs.push_back(root);

                                    log("  [rewrite] factored %s in module '%s': "
                                        "%s(%s, %s) -> %s(%s, %s)\n",
                                        common.c_str(),
                                        m.name.c_str(),
                                        MyIR::optype_to_string(m.exprs[idx].op),
                                        m.exprs[idx].operands[0].c_str(),
                                        m.exprs[idx].operands[1].c_str(),
                                        MyIR::optype_to_string(root.op),
                                        root.operands[0].c_str(),
                                        root.operands[1].c_str());

                                    total++;
                                    changed = true;
                                    rewritten = true;
                                }
                            }
                        }
                    }

                    if (!rewritten)
                        rewritten_exprs.push_back(root);
                }

                m.exprs.swap(rewritten_exprs);
            }
        }

        log("  Applied %d rewrite(s).\n", total);
    }
} MyIRRewritePass;

PRIVATE_NAMESPACE_END
