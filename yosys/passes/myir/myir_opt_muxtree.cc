#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <map>
#include <set>
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

struct BranchState {
    bool live_a = false;
    bool live_b = false;
};

struct ConditionState {
    std::map<std::string, bool> known_values;
    bool conflict = false;
};

static bool const_as_bool(const std::string &name)
{
    return MyIR::const_to_uint(name) != 0;
}

static std::unordered_map<std::string, int> build_producer_map(const MyIR::Module &m)
{
    std::unordered_map<std::string, int> producer;
    for (int i = 0; i < (int)m.exprs.size(); i++)
        producer[m.exprs[i].result_wire] = i;
    return producer;
}

static std::unordered_map<std::string, std::string> build_connect_map(const MyIR::Module &m)
{
    std::unordered_map<std::string, std::string> connect_map;
    for (const auto &c : m.connects)
        if (!c.lhs.empty() && !MyIR::is_constant_name(c.lhs))
            connect_map[c.lhs] = c.rhs;
    return connect_map;
}

static bool resolve_passthrough_signal(
    const MyIR::Module &m,
    const std::unordered_map<std::string, int> &producer,
    const std::unordered_map<std::string, std::string> &connect_map,
    const std::string &signal,
    std::string &resolved)
{
    std::set<std::string> seen;
    resolved = signal;

    while (!MyIR::is_constant_name(resolved)) {
        if (!seen.insert(resolved).second)
            return false;

        auto conn_it = connect_map.find(resolved);
        if (conn_it != connect_map.end() && conn_it->second != resolved) {
            resolved = conn_it->second;
            continue;
        }

        auto prod_it = producer.find(resolved);
        if (prod_it == producer.end())
            break;

        const MyIR::Expr &expr = m.exprs[prod_it->second];
        if ((expr.op == MyIR::OpType::Buf || expr.op == MyIR::OpType::Pos) &&
            expr.operands.size() == 1) {
            resolved = expr.operands[0];
            continue;
        }

        break;
    }

    return true;
}

static bool resolve_mux_source(
    const MyIR::Module &m,
    const std::unordered_map<std::string, int> &producer,
    const std::unordered_map<std::string, std::string> &connect_map,
    const std::string &signal,
    int &mux_idx)
{
    std::string resolved;
    if (!resolve_passthrough_signal(m, producer, connect_map, signal, resolved))
        return false;

    auto it = producer.find(resolved);
    if (it == producer.end() || m.exprs[it->second].op != MyIR::OpType::Mux)
        return false;

    mux_idx = it->second;
    return true;
}

static bool infer_signal_value(const MyIR::Module &m,
                               const std::unordered_map<std::string, int> &producer,
                               const std::unordered_map<std::string, std::string> &connect_map,
                               const ConditionState &cond,
                               const std::string &signal,
                               bool &value)
{
    if (MyIR::is_constant_name(signal)) {
        value = const_as_bool(signal);
        return true;
    }

    auto it = cond.known_values.find(signal);
    if (it != cond.known_values.end()) {
        value = it->second;
        return true;
    }

    auto conn_it = connect_map.find(signal);
    if (conn_it != connect_map.end() && conn_it->second != signal)
        return infer_signal_value(m, producer, connect_map, cond, conn_it->second, value);

    auto prod_it = producer.find(signal);
    if (prod_it == producer.end())
        return false;

    const MyIR::Expr &expr = m.exprs[prod_it->second];
    if (expr.op == MyIR::OpType::Not && expr.operands.size() == 1) {
        bool inner;
        if (!infer_signal_value(m, producer, connect_map, cond, expr.operands[0], inner))
            return false;
        value = !inner;
        return true;
    }

    if ((expr.op == MyIR::OpType::Buf || expr.op == MyIR::OpType::Pos) &&
        expr.operands.size() == 1)
        return infer_signal_value(m, producer, connect_map, cond, expr.operands[0], value);

    return false;
}

static ConditionState add_known_value(const MyIR::Module &m,
                                      const std::unordered_map<std::string, int> &producer,
                                      const std::unordered_map<std::string, std::string> &connect_map,
                                      ConditionState cond,
                                      const std::string &signal,
                                      bool value)
{
    if (cond.conflict)
        return cond;

    if (MyIR::is_constant_name(signal)) {
        if (const_as_bool(signal) != value)
            cond.conflict = true;
        return cond;
    }

    auto it = cond.known_values.find(signal);
    if (it != cond.known_values.end()) {
        if (it->second != value)
            cond.conflict = true;
        return cond;
    }

    cond.known_values[signal] = value;

    auto conn_it = connect_map.find(signal);
    if (conn_it != connect_map.end() && conn_it->second != signal)
        return add_known_value(m, producer, connect_map, cond, conn_it->second, value);

    auto prod_it = producer.find(signal);
    if (prod_it == producer.end())
        return cond;

    const MyIR::Expr &expr = m.exprs[prod_it->second];
    if (expr.op == MyIR::OpType::Not && expr.operands.size() == 1)
        return add_known_value(m, producer, connect_map, cond, expr.operands[0], !value);

    if ((expr.op == MyIR::OpType::Buf || expr.op == MyIR::OpType::Pos) &&
        expr.operands.size() == 1)
        return add_known_value(m, producer, connect_map, cond, expr.operands[0], value);

    return cond;
}

static void visit_mux_tree(const MyIR::Module &m, int mux_idx,
                           const std::unordered_map<std::string, int> &producer,
                           const std::unordered_map<std::string, std::string> &connect_map,
                           std::map<int, BranchState> &branch_states,
                           const ConditionState &path_cond)
{
    if (path_cond.conflict)
        return;

    const MyIR::Expr &mux = m.exprs[mux_idx];
    if (mux.op != MyIR::OpType::Mux || mux.operands.size() != 3)
        return;

    const std::string &a = mux.operands[0];
    const std::string &b = mux.operands[1];
    const std::string &s = mux.operands[2];

    auto recurse_operand = [&](const std::string &operand,
                               const ConditionState &next_cond) {
        int child_mux_idx = -1;
        if (!resolve_mux_source(m, producer, connect_map, operand, child_mux_idx))
            return;
        visit_mux_tree(m, child_mux_idx, producer, connect_map, branch_states, next_cond);
    };

    if (MyIR::is_constant_name(s)) {
        if (const_as_bool(s)) {
            branch_states[mux_idx].live_b = true;
            recurse_operand(b, path_cond);
        } else {
            branch_states[mux_idx].live_a = true;
            recurse_operand(a, path_cond);
        }
        return;
    }

    bool select_value = false;
    if (infer_signal_value(m, producer, connect_map, path_cond, s, select_value)) {
        if (select_value) {
            branch_states[mux_idx].live_b = true;
            recurse_operand(b, path_cond);
        } else {
            branch_states[mux_idx].live_a = true;
            recurse_operand(a, path_cond);
        }
        return;
    }

    branch_states[mux_idx].live_a = true;
    branch_states[mux_idx].live_b = true;

    auto cond_a = add_known_value(m, producer, connect_map, path_cond, s, false);
    recurse_operand(a, cond_a);

    auto cond_b = add_known_value(m, producer, connect_map, path_cond, s, true);
    recurse_operand(b, cond_b);
}

static std::set<int> find_root_muxes(const MyIR::Module &m,
                                     const std::unordered_map<std::string, int> &producer,
                                     const std::unordered_map<std::string, std::string> &connect_map)
{
    std::unordered_map<int, int> mux_parent_uses;
    std::set<int> roots;

    for (int i = 0; i < (int)m.exprs.size(); i++) {
        const MyIR::Expr &expr = m.exprs[i];
        for (const auto &operand : expr.operands) {
            int child_mux_idx = -1;
            if (!resolve_mux_source(m, producer, connect_map, operand, child_mux_idx))
                continue;
            if (expr.op == MyIR::OpType::Mux)
                mux_parent_uses[child_mux_idx]++;
            else
                roots.insert(child_mux_idx);
        }
    }

    for (const auto &conn : m.connects) {
        int mux_idx = -1;
        if (resolve_mux_source(m, producer, connect_map, conn.rhs, mux_idx))
            roots.insert(mux_idx);
    }

    for (int i = 0; i < (int)m.exprs.size(); i++) {
        const MyIR::Expr &expr = m.exprs[i];
        if (expr.op != MyIR::OpType::Mux)
            continue;

        auto w_it = m.wires.find(expr.result_wire);
        if (w_it != m.wires.end()) {
            MyIR::PortDir d = w_it->second.dir;
            if (d == MyIR::PortDir::Output || d == MyIR::PortDir::InOut)
                roots.insert(i);
        }

        if (!mux_parent_uses.count(i))
            roots.insert(i);
    }

    return roots;
}

} // namespace

struct MyIROptMuxtreePass : public Pass {
    MyIROptMuxtreePass() : Pass("myir_opt_muxtree",
        "eliminate dead branches in MyIR mux trees") {}

    void help() override
    {
        log("\n");
        log("    myir_opt_muxtree\n");
        log("\n");
        log("Analyze cascaded MyIR Mux expressions as mux trees and remove\n");
        log("branches that can never be selected because the select signal\n");
        log("is already constrained by the path from an ancestor mux.\n");
        log("\n");
        log("The path-condition analysis also follows simple Not/Buf/Pos\n");
        log("select expressions, and muxes with identical A/B inputs are\n");
        log("collapsed as well.\n");
        log("\n");
        log("The 'benefit score' printed for each simplification is\n");
        log("advisory only -- mux simplification is unconditionally\n");
        log("profitable, so the score does NOT gate the rewrite.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_opt_muxtree.\n");
        extra_args(args, 1, design);

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded.\n");
            return;
        }

        int total_removed = 0;
        int total_mux_simplified = 0;
        int total_root_muxes = 0;
        int total_visits = 0;
        int cnt_same = 0;     // identical A/B
        int cnt_const = 0;    // constant select
        int cnt_path = 0;     // path-condition dead branch

        for (auto &mkv : myir->modules)
        {
            MyIR::Module &m = mkv.second;
            bool changed = true;

            while (changed) {
                changed = false;

                auto producer = build_producer_map(m);
                auto connect_map = build_connect_map(m);
                auto roots = find_root_muxes(m, producer, connect_map);
                std::map<int, BranchState> branch_states;

                total_root_muxes += (int)roots.size();

                for (int root_idx : roots) {
                    visit_mux_tree(m, root_idx, producer, connect_map, branch_states, ConditionState());
                    total_visits++;
                }

                std::vector<MyIR::Expr> new_exprs;
                new_exprs.reserve(m.exprs.size());

                for (int i = 0; i < (int)m.exprs.size(); i++)
                {
                    const MyIR::Expr &expr = m.exprs[i];
                    if (expr.op != MyIR::OpType::Mux || expr.operands.size() != 3) {
                        new_exprs.push_back(expr);
                        continue;
                    }

                    bool remove_mux = false;
                    std::string chosen_rhs;
                    int dead_branches = 0;
                    bool select_value = false;
                    int benefit_score = 0;
                    const char *rule_name = nullptr;

                    if (expr.operands[0] == expr.operands[1]) {
                        chosen_rhs = expr.operands[0];
                        dead_branches = 1;
                        benefit_score = MyIR::transform_benefit_score(
                            1, 1, 0, 0, 1);
                        remove_mux = true;          // advisory: score >0 anyway
                        rule_name = "same_inputs";
                        cnt_same++;
                    } else if (infer_signal_value(m, producer, connect_map, ConditionState(),
                                                  expr.operands[2], select_value)) {
                        chosen_rhs = select_value
                            ? expr.operands[1] : expr.operands[0];
                        dead_branches = 1;
                        benefit_score = MyIR::transform_benefit_score(
                            1, 1, 0, 0, 1);
                        remove_mux = true;
                        rule_name = "const_sel";
                        cnt_const++;
                    } else {
                        auto st_it = branch_states.find(i);
                        if (st_it != branch_states.end()) {
                            const BranchState &st = st_it->second;
                            if (st.live_a && !st.live_b) {
                                chosen_rhs = expr.operands[0];
                                dead_branches = 1;
                                benefit_score = MyIR::transform_benefit_score(
                                    1, 1, 0, 0, 1);
                                remove_mux = true;
                                rule_name = "path_cond";
                                cnt_path++;
                            } else if (!st.live_a && st.live_b) {
                                chosen_rhs = expr.operands[1];
                                dead_branches = 1;
                                benefit_score = MyIR::transform_benefit_score(
                                    1, 1, 0, 0, 1);
                                remove_mux = true;
                                rule_name = "path_cond";
                                cnt_path++;
                            }
                        }
                    }

                    if (remove_mux) {
                        MyIR::Assign a;
                        a.lhs = expr.result_wire;
                        a.rhs = chosen_rhs;
                        m.connects.push_back(a);

                        log("  [opt_muxtree] module '%s': removed dead mux branch on %s, "
                            "replaced with %s (rule=%s).\n",
                            m.name.c_str(),
                            expr.result_wire.c_str(),
                            chosen_rhs.c_str(),
                            rule_name ? rule_name : "?");
                        log("    benefit score = %d (advisory)\n", benefit_score);

                        total_removed += dead_branches;
                        total_mux_simplified++;
                        changed = true;
                    } else {
                        new_exprs.push_back(expr);
                    }
                }

                m.exprs.swap(new_exprs);
            }
        }

        log("  myir_opt_muxtree summary:\n");
        log("    root muxes discovered  : %d\n", total_root_muxes);
        log("    mux-tree visits        : %d\n", total_visits);
        log("    branches removed       : %d (same_inputs=%d / const_sel=%d / path_cond=%d)\n",
            total_removed, cnt_same, cnt_const, cnt_path);
        log("    muxes collapsed        : %d\n", total_mux_simplified);
    }
} MyIROptMuxtreePass;

PRIVATE_NAMESPACE_END
