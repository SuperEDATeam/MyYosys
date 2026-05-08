#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <map>
#include <set>
#include <unordered_map>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

struct CutInfo {
    std::set<std::string> leaves;
    std::set<int> nodes;
};

enum class RewriteKind { Redirect, Factor };

struct RewriteCandidate {
    RewriteKind kind = RewriteKind::Redirect;
    std::string rule_name;
    int score = 0;
    int root_idx = -1;
    int lhs_idx = -1;
    int rhs_idx = -1;
    CutInfo cut;

    // Redirect form: replace root by "result_wire = redirect_target"
    std::set<int> remove_nodes;
    std::string redirect_target;

    // Factor form: replace 3 nodes by 2 nodes
    std::string common;
    std::string lhs_other;
    std::string rhs_other;
};

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

static bool expr_contains_operand(const MyIR::Expr &expr,
                                  const std::string &needle,
                                  std::string &other)
{
    if (expr.operands.size() != 2)
        return false;

    if (expr.operands[0] == needle) {
        other = expr.operands[1];
        return true;
    }
    if (expr.operands[1] == needle) {
        other = expr.operands[0];
        return true;
    }

    return false;
}

static bool is_not_of(const MyIR::Module &m,
                      const std::unordered_map<std::string, int> &producer,
                      const std::unordered_map<std::string, int> &uses,
                      const std::string &wire,
                      std::string &base,
                      int *node_idx = nullptr)
{
    auto it = producer.find(wire);
    if (it == producer.end())
        return false;

    int idx = it->second;
    const MyIR::Expr &expr = m.exprs[idx];
    if (expr.op != MyIR::OpType::Not || expr.operands.size() != 1)
        return false;
    if (uses.count(expr.result_wire) && uses.at(expr.result_wire) != 1)
        return false;

    base = expr.operands[0];
    if (node_idx)
        *node_idx = idx;
    return true;
}

static std::string cut_key(const CutInfo &cut)
{
    std::string key;
    bool first = true;
    for (const auto &leaf : cut.leaves) {
        if (!first)
            key += "|";
        key += leaf;
        first = false;
    }

    key += "#";
    first = true;
    for (int node : cut.nodes) {
        if (!first)
            key += "|";
        key += std::to_string(node);
        first = false;
    }

    return key;
}

static void append_unique_cut(std::vector<CutInfo> &cuts, const CutInfo &cut)
{
    std::string key = cut_key(cut);
    for (const auto &existing : cuts)
        if (cut_key(existing) == key)
            return;

    cuts.push_back(cut);
}

static void combine_cut_options(const std::vector<std::vector<CutInfo>> &options,
                                int pos, int k, CutInfo current,
                                std::vector<CutInfo> &combined)
{
    if (pos == (int)options.size()) {
        append_unique_cut(combined, current);
        return;
    }

    for (const auto &opt : options[pos]) {
        CutInfo merged = current;
        merged.leaves.insert(opt.leaves.begin(), opt.leaves.end());
        if ((int)merged.leaves.size() > k)
            continue;

        merged.nodes.insert(opt.nodes.begin(), opt.nodes.end());
        combine_cut_options(options, pos + 1, k, merged, combined);
    }
}

static std::vector<CutInfo> enumerate_cuts_rec(
    int idx, const MyIR::Module &m, int k,
    const std::unordered_map<std::string, int> &producer,
    std::map<int, std::vector<CutInfo>> &memo)
{
    auto memo_it = memo.find(idx);
    if (memo_it != memo.end())
        return memo_it->second;

    const MyIR::Expr &expr = m.exprs[idx];
    std::vector<std::vector<CutInfo>> operand_options;

    for (const auto &operand : expr.operands) {
        std::vector<CutInfo> opts;

        CutInfo trivial_leaf;
        trivial_leaf.leaves.insert(operand);
        opts.push_back(trivial_leaf);

        auto prod_it = producer.find(operand);
        if (prod_it != producer.end()) {
            std::vector<CutInfo> child_cuts =
                enumerate_cuts_rec(prod_it->second, m, k, producer, memo);
            opts.insert(opts.end(), child_cuts.begin(), child_cuts.end());
        }

        operand_options.push_back(opts);
    }

    std::vector<CutInfo> cuts;
    combine_cut_options(operand_options, 0, k, CutInfo(), cuts);

    for (auto &cut : cuts)
        cut.nodes.insert(idx);

    memo[idx] = cuts;
    return cuts;
}

static std::map<int, std::vector<CutInfo>> enumerate_all_cuts(
    const MyIR::Module &m, int k,
    const std::unordered_map<std::string, int> &producer)
{
    std::map<int, std::vector<CutInfo>> memo;
    for (int i = 0; i < (int)m.exprs.size(); i++)
        enumerate_cuts_rec(i, m, k, producer, memo);
    return memo;
}

static int count_removed_muxes(const MyIR::Module &m, const std::set<int> &nodes)
{
    int removed_muxes = 0;
    for (int idx : nodes)
        if (idx >= 0 && idx < (int)m.exprs.size() &&
            m.exprs[idx].op == MyIR::OpType::Mux)
            removed_muxes++;
    return removed_muxes;
}

static int count_total_cuts(const std::map<int, std::vector<CutInfo>> &all_cuts)
{
    int total = 0;
    for (const auto &it : all_cuts)
        total += (int)it.second.size();
    return total;
}

static bool is_zero_const(const std::string &name, int width)
{
    return MyIR::is_constant_name(name) &&
           MyIR::const_width(name) == width &&
           MyIR::const_to_uint(name) == 0;
}

static std::string make_allones_const(int width)
{
    if (width <= 0)
        width = 1;
    return std::to_string(width) + "'" + std::string(width, '1');
}

static bool is_allones_const(const std::string &name, int width)
{
    if (!MyIR::is_constant_name(name) || MyIR::const_width(name) != width)
        return false;

    if (width >= 64)
        return false;

    uint64_t mask = (1ULL << width) - 1;
    return MyIR::const_to_uint(name) == mask;
}

static bool try_factor_rule(const MyIR::Module &m, int idx, int k,
                            const std::unordered_map<std::string, int> &producer,
                            const std::unordered_map<std::string, int> &uses,
                            const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if (!is_factorable_parent(root.op) || root.operands.size() != 2)
        return false;

    auto it_l = producer.find(root.operands[0]);
    auto it_r = producer.find(root.operands[1]);
    if (it_l == producer.end() || it_r == producer.end())
        return false;
    if (it_l->second == it_r->second)
        return false;

    int lhs_idx = it_l->second;
    int rhs_idx = it_r->second;
    const MyIR::Expr &lhs = m.exprs[lhs_idx];
    const MyIR::Expr &rhs = m.exprs[rhs_idx];

    if (lhs.result_width != root.result_width ||
        rhs.result_width != root.result_width)
        return false;
    if (!is_factorable_child(root.op, lhs.op) ||
        !is_factorable_child(root.op, rhs.op))
        return false;
    if (uses.count(lhs.result_wire) && uses.at(lhs.result_wire) != 1)
        return false;
    if (uses.count(rhs.result_wire) && uses.at(rhs.result_wire) != 1)
        return false;
    if ((int)cut.leaves.size() > k)
        return false;
    if (!cut.nodes.count(idx) || !cut.nodes.count(lhs_idx) || !cut.nodes.count(rhs_idx))
        return false;

    std::string common, lhs_other, rhs_other;
    if (!extract_common_operand(lhs, rhs, common, lhs_other, rhs_other))
        return false;
    if (lhs_other == rhs_other)
        return false;

    std::set<std::string> expected_leaves;
    expected_leaves.insert(common);
    expected_leaves.insert(lhs_other);
    expected_leaves.insert(rhs_other);
    if (expected_leaves != cut.leaves)
        return false;

    cand.kind = RewriteKind::Factor;
    cand.rule_name = root.op == MyIR::OpType::Or ? "distributive_factor_or"
                                                 : "distributive_factor_and";
    cand.root_idx = idx;
    cand.lhs_idx = lhs_idx;
    cand.rhs_idx = rhs_idx;
    cand.cut = cut;
    cand.common = common;
    cand.lhs_other = lhs_other;
    cand.rhs_other = rhs_other;
    cand.score = MyIR::transform_benefit_score(
        3, count_removed_muxes(m, {idx, lhs_idx, rhs_idx}), 2, 1);
    return true;
}

static bool try_absorption_rule(const MyIR::Module &m, int idx,
                                const std::unordered_map<std::string, int> &producer,
                                const std::unordered_map<std::string, int> &uses,
                                const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if ((root.op != MyIR::OpType::Or && root.op != MyIR::OpType::And) ||
        root.operands.size() != 2)
        return false;

    for (int side = 0; side < 2; side++) {
        const std::string &direct = root.operands[side];
        const std::string &nested_wire = root.operands[1 - side];
        auto nested_it = producer.find(nested_wire);
        if (nested_it == producer.end())
            continue;

        int nested_idx = nested_it->second;
        const MyIR::Expr &nested = m.exprs[nested_idx];
        if (nested.result_width != root.result_width)
            continue;

        bool dual_match = (root.op == MyIR::OpType::Or  && nested.op == MyIR::OpType::And) ||
                          (root.op == MyIR::OpType::And && nested.op == MyIR::OpType::Or);
        if (!dual_match)
            continue;
        if (uses.count(nested.result_wire) && uses.at(nested.result_wire) != 1)
            continue;
        if (!cut.nodes.count(idx) || !cut.nodes.count(nested_idx))
            continue;

        std::string other;
        if (!expr_contains_operand(nested, direct, other))
            continue;

        std::set<std::string> expected_leaves;
        expected_leaves.insert(direct);
        expected_leaves.insert(other);
        if (expected_leaves != cut.leaves)
            continue;

        cand.kind = RewriteKind::Redirect;
        cand.rule_name = root.op == MyIR::OpType::Or ? "absorption_or"
                                                     : "absorption_and";
        cand.root_idx = idx;
        cand.cut = cut;
        cand.remove_nodes = {idx, nested_idx};
        cand.redirect_target = direct;
        cand.score = MyIR::transform_benefit_score(
            2, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
        return true;
    }

    return false;
}

static bool try_idempotent_rule(const MyIR::Module &m, int idx,
                                const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if ((root.op != MyIR::OpType::Or && root.op != MyIR::OpType::And) ||
        root.operands.size() != 2)
        return false;
    if (root.operands[0] != root.operands[1])
        return false;
    if (cut.leaves.size() != 1 || !cut.leaves.count(root.operands[0]))
        return false;

    cand.kind = RewriteKind::Redirect;
    cand.rule_name = root.op == MyIR::OpType::Or ? "idempotent_or"
                                                 : "idempotent_and";
    cand.root_idx = idx;
    cand.cut = cut;
    cand.remove_nodes = {idx};
    cand.redirect_target = root.operands[0];
    cand.score = MyIR::transform_benefit_score(
        1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
    return true;
}

static bool try_xor_self_rule(const MyIR::Module &m, int idx,
                              const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if ((root.op != MyIR::OpType::Xor && root.op != MyIR::OpType::Xnor) ||
        root.operands.size() != 2)
        return false;
    if (root.operands[0] != root.operands[1])
        return false;
    if (cut.leaves.size() != 1 || !cut.leaves.count(root.operands[0]))
        return false;

    cand.kind = RewriteKind::Redirect;
    cand.rule_name = root.op == MyIR::OpType::Xor ? "xor_self_zero"
                                                  : "xnor_self_one";
    cand.root_idx = idx;
    cand.cut = cut;
    cand.remove_nodes = {idx};
    cand.redirect_target = root.op == MyIR::OpType::Xor
        ? MyIR::uint_to_const(0, root.result_width)
        : make_allones_const(root.result_width);
    cand.score = MyIR::transform_benefit_score(
        1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
    return true;
}

static bool try_not_not_rule(const MyIR::Module &m, int idx,
                             const std::unordered_map<std::string, int> &producer,
                             const std::unordered_map<std::string, int> &uses,
                             const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if (root.op != MyIR::OpType::Not || root.operands.size() != 1)
        return false;

    auto child_it = producer.find(root.operands[0]);
    if (child_it == producer.end())
        return false;

    int child_idx = child_it->second;
    const MyIR::Expr &child = m.exprs[child_idx];
    if (child.op != MyIR::OpType::Not || child.operands.size() != 1)
        return false;
    if (child.result_width != root.result_width)
        return false;
    if (uses.count(child.result_wire) && uses.at(child.result_wire) != 1)
        return false;
    if (!cut.nodes.count(idx) || !cut.nodes.count(child_idx))
        return false;
    if (cut.leaves.size() != 1 || !cut.leaves.count(child.operands[0]))
        return false;

    cand.kind = RewriteKind::Redirect;
    cand.rule_name = "double_not";
    cand.root_idx = idx;
    cand.cut = cut;
    cand.remove_nodes = {idx, child_idx};
    cand.redirect_target = child.operands[0];
    cand.score = MyIR::transform_benefit_score(
        2, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
    return true;
}

static bool try_const_rule(const MyIR::Module &m, int idx,
                           const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if (root.operands.size() != 2)
        return false;

    const std::string &a = root.operands[0];
    const std::string &b = root.operands[1];
    const int width = root.result_width;

    if (root.op == MyIR::OpType::And) {
        if (is_zero_const(a, width) || is_zero_const(b, width)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "and_zero";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = MyIR::uint_to_const(0, width);
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
        if (is_allones_const(a, width) && !MyIR::is_constant_name(b)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "and_allones";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = b;
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
        if (is_allones_const(b, width) && !MyIR::is_constant_name(a)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "and_allones";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = a;
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
    }

    if (root.op == MyIR::OpType::Or) {
        if (is_allones_const(a, width) || is_allones_const(b, width)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "or_allones";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = make_allones_const(width);
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
        if (is_zero_const(a, width) && !MyIR::is_constant_name(b)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "or_zero";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = b;
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
        if (is_zero_const(b, width) && !MyIR::is_constant_name(a)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "or_zero";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = a;
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
    }

    if (root.op == MyIR::OpType::Xor) {
        if (is_zero_const(a, width) && !MyIR::is_constant_name(b)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "xor_zero";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = b;
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
        if (is_zero_const(b, width) && !MyIR::is_constant_name(a)) {
            cand.kind = RewriteKind::Redirect;
            cand.rule_name = "xor_zero";
            cand.root_idx = idx;
            cand.cut = cut;
            cand.remove_nodes = {idx};
            cand.redirect_target = a;
            cand.score = MyIR::transform_benefit_score(
                1, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
            return true;
        }
    }

    return false;
}

static bool try_demorgan_rule(const MyIR::Module &m, int idx,
                              const std::unordered_map<std::string, int> &producer,
                              const std::unordered_map<std::string, int> &uses,
                              const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if ((root.op != MyIR::OpType::Or && root.op != MyIR::OpType::And) ||
        root.operands.size() != 2)
        return false;

    std::string a, b;
    int na_idx = -1, nb_idx = -1;
    if (!is_not_of(m, producer, uses, root.operands[0], a, &na_idx))
        return false;
    if (!is_not_of(m, producer, uses, root.operands[1], b, &nb_idx))
        return false;
    if (!cut.nodes.count(idx) || !cut.nodes.count(na_idx) || !cut.nodes.count(nb_idx))
        return false;

    std::set<std::string> expected_leaves;
    expected_leaves.insert(a);
    expected_leaves.insert(b);
    if (cut.leaves != expected_leaves)
        return false;

    cand.kind = RewriteKind::Factor;
    cand.rule_name = root.op == MyIR::OpType::Or ? "demorgan_or"
                                                 : "demorgan_and";
    cand.root_idx = idx;
    cand.lhs_idx = na_idx;
    cand.rhs_idx = nb_idx;
    cand.cut = cut;
    cand.common.clear();
    cand.lhs_other = a;
    cand.rhs_other = b;
    cand.score = MyIR::transform_benefit_score(
        3, count_removed_muxes(m, {idx, na_idx, nb_idx}), 2, 1);
    return true;
}

static bool try_consensus_rule(const MyIR::Module &m, int idx,
                               const std::unordered_map<std::string, int> &producer,
                               const std::unordered_map<std::string, int> &uses,
                               const CutInfo &cut, RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[idx];
    if ((root.op != MyIR::OpType::Or && root.op != MyIR::OpType::And) ||
        root.operands.size() != 2)
        return false;

    for (int side = 0; side < 2; side++) {
        const std::string &direct = root.operands[side];
        const std::string &nested_wire = root.operands[1 - side];
        auto nested_it = producer.find(nested_wire);
        if (nested_it == producer.end())
            continue;

        int nested_idx = nested_it->second;
        const MyIR::Expr &nested = m.exprs[nested_idx];
        if (nested.result_width != root.result_width)
            continue;
        if (uses.count(nested.result_wire) && uses.at(nested.result_wire) != 1)
            continue;
        if (!cut.nodes.count(idx) || !cut.nodes.count(nested_idx))
            continue;

        if (root.op == MyIR::OpType::Or && nested.op == MyIR::OpType::And) {
            std::string x, y, neg_base;
            int not_idx = -1;

            if (is_not_of(m, producer, uses, nested.operands[0], neg_base, &not_idx) &&
                nested.operands[1] != direct &&
                neg_base == direct) {
                x = direct;
                y = nested.operands[1];
            } else if (is_not_of(m, producer, uses, nested.operands[1], neg_base, &not_idx) &&
                       nested.operands[0] != direct &&
                       neg_base == direct) {
                x = direct;
                y = nested.operands[0];
            } else {
                continue;
            }

            if (!cut.nodes.count(not_idx))
                continue;
            std::set<std::string> expected_leaves = {x, y};
            if (cut.leaves != expected_leaves)
                continue;

            cand.kind = RewriteKind::Factor;
            cand.rule_name = "resub_or";
            cand.root_idx = idx;
            cand.lhs_idx = not_idx;
            cand.rhs_idx = nested_idx;
            cand.cut = cut;
            cand.common.clear();
            cand.lhs_other = x;
            cand.rhs_other = y;
            cand.score = MyIR::transform_benefit_score(
                3, count_removed_muxes(m, {idx, not_idx, nested_idx}), 2, 1);
            return true;
        }

        if (root.op == MyIR::OpType::And && nested.op == MyIR::OpType::Or) {
            std::string x, y, neg_base;
            int not_idx = -1;

            if (is_not_of(m, producer, uses, nested.operands[0], neg_base, &not_idx) &&
                nested.operands[1] != direct &&
                neg_base == direct) {
                x = direct;
                y = nested.operands[1];
            } else if (is_not_of(m, producer, uses, nested.operands[1], neg_base, &not_idx) &&
                       nested.operands[0] != direct &&
                       neg_base == direct) {
                x = direct;
                y = nested.operands[0];
            } else {
                continue;
            }

            if (!cut.nodes.count(not_idx))
                continue;
            std::set<std::string> expected_leaves = {x, y};
            if (cut.leaves != expected_leaves)
                continue;

            cand.kind = RewriteKind::Factor;
            cand.rule_name = "resub_and";
            cand.root_idx = idx;
            cand.lhs_idx = not_idx;
            cand.rhs_idx = nested_idx;
            cand.cut = cut;
            cand.common.clear();
            cand.lhs_other = x;
            cand.rhs_other = y;
            cand.score = MyIR::transform_benefit_score(
                3, count_removed_muxes(m, {idx, not_idx, nested_idx}), 2, 1);
            return true;
        }
    }

    for (int lhs_side = 0; lhs_side < 2; lhs_side++) {
        auto lhs_it = producer.find(root.operands[lhs_side]);
        auto rhs_it = producer.find(root.operands[1 - lhs_side]);
        if (lhs_it == producer.end() || rhs_it == producer.end())
            continue;

        int lhs_idx = lhs_it->second;
        int rhs_idx = rhs_it->second;
        const MyIR::Expr &lhs = m.exprs[lhs_idx];
        const MyIR::Expr &rhs = m.exprs[rhs_idx];

        if (uses.count(lhs.result_wire) && uses.at(lhs.result_wire) != 1)
            continue;
        if (uses.count(rhs.result_wire) && uses.at(rhs.result_wire) != 1)
            continue;
        if (!cut.nodes.count(idx) || !cut.nodes.count(lhs_idx) || !cut.nodes.count(rhs_idx))
            continue;

        if (root.op == MyIR::OpType::Or &&
            lhs.op == MyIR::OpType::And &&
            rhs.op == MyIR::OpType::And) {
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 2; j++) {
                    std::string x, y;
                    int not_idx = -1;
                    if (!is_not_of(m, producer, uses, rhs.operands[j], x, &not_idx))
                        continue;
                    if (lhs.operands[i] != x)
                        continue;

                    y = lhs.operands[1 - i];
                    if (rhs.operands[1 - j] != y)
                        continue;
                    if (!cut.nodes.count(not_idx))
                        continue;

                    std::set<std::string> expected_leaves = {x, y};
                    if (cut.leaves != expected_leaves)
                        continue;

                    cand.kind = RewriteKind::Redirect;
                    cand.rule_name = "consensus_or";
                    cand.root_idx = idx;
                    cand.cut = cut;
                    cand.remove_nodes = {idx, lhs_idx, rhs_idx, not_idx};
                    cand.redirect_target = y;
                    cand.score = MyIR::transform_benefit_score(
                        4, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
                    return true;
                }
            }
        }

        if (root.op == MyIR::OpType::And &&
            lhs.op == MyIR::OpType::Or &&
            rhs.op == MyIR::OpType::Or) {
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 2; j++) {
                    std::string x, y;
                    int not_idx = -1;
                    if (!is_not_of(m, producer, uses, rhs.operands[j], x, &not_idx))
                        continue;
                    if (lhs.operands[i] != x)
                        continue;

                    y = lhs.operands[1 - i];
                    if (rhs.operands[1 - j] != y)
                        continue;
                    if (!cut.nodes.count(not_idx))
                        continue;

                    std::set<std::string> expected_leaves = {x, y};
                    if (cut.leaves != expected_leaves)
                        continue;

                    cand.kind = RewriteKind::Redirect;
                    cand.rule_name = "consensus_and";
                    cand.root_idx = idx;
                    cand.cut = cut;
                    cand.remove_nodes = {idx, lhs_idx, rhs_idx, not_idx};
                    cand.redirect_target = y;
                    cand.score = MyIR::transform_benefit_score(
                        4, count_removed_muxes(m, cand.remove_nodes), 0, 0, 1);
                    return true;
                }
            }
        }
    }

    return false;
}

static bool find_rewrite_candidate(
    const MyIR::Module &m, int k,
    const std::unordered_map<std::string, int> &producer,
    const std::unordered_map<std::string, int> &uses,
    const std::map<int, std::vector<CutInfo>> &all_cuts,
    RewriteCandidate &cand)
{
    bool found = false;
    RewriteCandidate best;

    for (int idx = 0; idx < (int)m.exprs.size(); idx++)
    {
        auto cut_it = all_cuts.find(idx);
        if (cut_it == all_cuts.end())
            continue;

        const MyIR::Expr &root = m.exprs[idx];
        for (const auto &cut : cut_it->second)
        {
            if ((int)cut.leaves.size() > k)
                continue;

            RewriteCandidate current;
            bool matched =
                try_demorgan_rule(m, idx, producer, uses, cut, current) ||
                try_consensus_rule(m, idx, producer, uses, cut, current) ||
                try_factor_rule(m, idx, k, producer, uses, cut, current) ||
                try_absorption_rule(m, idx, producer, uses, cut, current) ||
                try_not_not_rule(m, idx, producer, uses, cut, current) ||
                try_idempotent_rule(m, idx, cut, current) ||
                try_xor_self_rule(m, idx, cut, current) ||
                try_const_rule(m, idx, cut, current);

            if (!matched || current.score <= 0)
                continue;

            if (!found || current.score > best.score) {
                best = current;
                found = true;
            }
        }
    }

    if (found)
        cand = best;
    return found;
}

static void apply_redirect_rewrite(MyIR::Module &m, const RewriteCandidate &cand)
{
    const MyIR::Expr &root = m.exprs[cand.root_idx];

    std::vector<MyIR::Expr> new_exprs;
    new_exprs.reserve(m.exprs.size());
    for (int i = 0; i < (int)m.exprs.size(); i++)
        if (!cand.remove_nodes.count(i))
            new_exprs.push_back(m.exprs[i]);
    m.exprs.swap(new_exprs);

    if (root.result_wire != cand.redirect_target) {
        MyIR::Assign a;
        a.lhs = root.result_wire;
        a.rhs = cand.redirect_target;
        m.connects.push_back(a);
    }

    log("  [rewrite] k-cut{%d} %s in module '%s': %s -> %s\n",
        (int)cand.cut.leaves.size(),
        cand.rule_name.c_str(),
        m.name.c_str(),
        root.result_wire.c_str(),
        cand.redirect_target.c_str());
    log("    benefit score = %d\n", cand.score);
}

static void apply_factor_rewrite(MyIR::Module &m, const RewriteCandidate &cand,
                                 int &rewrite_counter)
{
    const MyIR::Expr old_root = m.exprs[cand.root_idx];

    if (cand.rule_name == "demorgan_or" || cand.rule_name == "demorgan_and") {
        std::string temp = fresh_wire_name(m, rewrite_counter);

        MyIR::Port w;
        w.name = temp;
        w.dir = MyIR::PortDir::Internal;
        w.width = old_root.result_width;
        m.wires[temp] = w;

        MyIR::Expr inner;
        inner.op = old_root.op == MyIR::OpType::Or ? MyIR::OpType::And
                                                   : MyIR::OpType::Or;
        inner.result_wire = temp;
        inner.result_width = old_root.result_width;
        inner.operands.push_back(cand.lhs_other);
        inner.operands.push_back(cand.rhs_other);

        MyIR::Expr new_root;
        new_root.op = MyIR::OpType::Not;
        new_root.result_wire = old_root.result_wire;
        new_root.result_width = old_root.result_width;
        new_root.operands.push_back(temp);

        std::vector<MyIR::Expr> new_exprs;
        new_exprs.reserve(m.exprs.size() - 1);

        for (int i = 0; i < (int)m.exprs.size(); i++) {
            if (i == cand.lhs_idx || i == cand.rhs_idx)
                continue;

            if (i == cand.root_idx) {
                new_exprs.push_back(inner);
                new_exprs.push_back(new_root);
            } else {
                new_exprs.push_back(m.exprs[i]);
            }
        }

        m.exprs.swap(new_exprs);

        log("  [rewrite] k-cut{%d} %s in module '%s': "
            "%s(%s, %s) -> Not(%s)\n",
            (int)cand.cut.leaves.size(),
            cand.rule_name.c_str(),
            m.name.c_str(),
            MyIR::optype_to_string(old_root.op),
            old_root.operands[0].c_str(),
            old_root.operands[1].c_str(),
            temp.c_str());
        log("    benefit score = %d\n", cand.score);
        return;
    }

    if (cand.rule_name == "resub_or" || cand.rule_name == "resub_and") {
        std::string temp = fresh_wire_name(m, rewrite_counter);

        MyIR::Port w;
        w.name = temp;
        w.dir = MyIR::PortDir::Internal;
        w.width = old_root.result_width;
        m.wires[temp] = w;

        MyIR::Expr inner;
        inner.op = cand.rule_name == "resub_or" ? MyIR::OpType::Or
                                                : MyIR::OpType::And;
        inner.result_wire = temp;
        inner.result_width = old_root.result_width;
        inner.operands.push_back(cand.lhs_other);
        inner.operands.push_back(cand.rhs_other);

        std::vector<MyIR::Expr> new_exprs;
        new_exprs.reserve(m.exprs.size() - 1);
        std::set<int> remove_nodes = {cand.rhs_idx, cand.lhs_idx};

        for (int i = 0; i < (int)m.exprs.size(); i++) {
            if (remove_nodes.count(i))
                continue;

            if (i == cand.root_idx) {
                MyIR::Expr new_root;
                new_root.op = MyIR::OpType::Buf;
                new_root.result_wire = old_root.result_wire;
                new_root.result_width = old_root.result_width;
                new_root.operands.push_back(temp);
                new_exprs.push_back(inner);
                new_exprs.push_back(new_root);
            } else {
                new_exprs.push_back(m.exprs[i]);
            }
        }

        m.exprs.swap(new_exprs);

        log("  [rewrite] k-cut{%d} %s in module '%s': result uses %s(%s, %s)\n",
            (int)cand.cut.leaves.size(),
            cand.rule_name.c_str(),
            m.name.c_str(),
            MyIR::optype_to_string(inner.op),
            cand.lhs_other.c_str(),
            cand.rhs_other.c_str());
        log("    benefit score = %d\n", cand.score);
        return;
    }

    const MyIR::Expr old_lhs = m.exprs[cand.lhs_idx];

    std::string temp = fresh_wire_name(m, rewrite_counter);

    MyIR::Port w;
    w.name = temp;
    w.dir = MyIR::PortDir::Internal;
    w.width = old_root.result_width;
    m.wires[temp] = w;

    MyIR::Expr inner;
    inner.op = old_root.op;
    inner.result_wire = temp;
    inner.result_width = old_root.result_width;
    inner.operands.push_back(cand.lhs_other);
    inner.operands.push_back(cand.rhs_other);

    MyIR::Expr new_root = old_root;
    new_root.op = old_lhs.op;
    new_root.operands.clear();
    new_root.operands.push_back(cand.common);
    new_root.operands.push_back(temp);

    std::vector<MyIR::Expr> new_exprs;
    new_exprs.reserve(m.exprs.size() - 1);

    for (int i = 0; i < (int)m.exprs.size(); i++) {
        if (i == cand.lhs_idx || i == cand.rhs_idx)
            continue;

        if (i == cand.root_idx) {
            new_exprs.push_back(inner);
            new_exprs.push_back(new_root);
        } else {
            new_exprs.push_back(m.exprs[i]);
        }
    }

    m.exprs.swap(new_exprs);

    log("  [rewrite] k-cut{%d} %s in module '%s': "
        "%s(%s, %s) -> %s(%s, %s)\n",
        (int)cand.cut.leaves.size(),
        cand.rule_name.c_str(),
        m.name.c_str(),
        MyIR::optype_to_string(old_root.op),
        old_root.operands[0].c_str(),
        old_root.operands[1].c_str(),
        MyIR::optype_to_string(new_root.op),
        new_root.operands[0].c_str(),
        new_root.operands[1].c_str());
    log("    benefit score = %d\n", cand.score);
}

static void apply_rewrite(MyIR::Module &m, const RewriteCandidate &cand,
                          int &rewrite_counter)
{
    if (cand.kind == RewriteKind::Factor)
        apply_factor_rewrite(m, cand, rewrite_counter);
    else
        apply_redirect_rewrite(m, cand);
}

} // namespace

struct MyIRRewritePass : public Pass {
    MyIRRewritePass() : Pass("myir_rewrite",
        "k-cut based algebraic rewriting on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_rewrite [-k N]\n");
        log("\n");
        log("Enumerate k-cuts for each MyIR expression and run local\n");
        log("pattern-based rewriting on the cuts.\n");
        log("\n");
        log("Options:\n");
        log("    -k N\n");
        log("        Maximum number of distinct leaves allowed in a cut.\n");
        log("        Default: 4.\n");
        log("\n");
        log("Currently implemented rewrite rules:\n");
        log("  (a & b) | (a & c)  ->  a & (b | c)\n");
        log("  (a | b) & (a | c)  ->  a | (b & c)\n");
        log("  ~a | ~b            ->  ~(a & b)\n");
        log("  ~a & ~b            ->  ~(a | b)\n");
        log("  (x & y) | (~x & y) ->  y\n");
        log("  (x | y) & (~x | y) ->  y\n");
        log("  y | (~x & y)       ->  x | y\n");
        log("  y & (~x | y)       ->  x & y\n");
        log("  x | (x & y)        ->  x\n");
        log("  x & (x | y)        ->  x\n");
        log("  x | x              ->  x\n");
        log("  x & x              ->  x\n");
        log("  x ^ x              ->  0\n");
        log("  x ~^ x             ->  1\n");
        log("  !!x                ->  x\n");
        log("  x & 0 / x | 0 / x & 1 / x | 1 / x ^ 0\n");
        log("\n");
        log("The pass precomputes cuts up to k leaves, scores all matching\n");
        log("local rewrites, and applies the highest-benefit positive\n");
        log("candidate. The process is repeated until no more profitable\n");
        log("rule matches remain.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_rewrite.\n");

        int k = 4;
        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++)
        {
            if (args[argidx] == "-k" && argidx + 1 < args.size()) {
                k = atoi(args[++argidx].c_str());
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        if (k < 2)
            log_error("myir_rewrite: -k must be >= 2.\n");

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

            while (true) {
                auto producer = build_producer_map(m);
                auto uses = build_use_count(m);
                auto all_cuts = enumerate_all_cuts(m, k, producer);
                RewriteCandidate cand;

                if (!find_rewrite_candidate(m, k, producer, uses, all_cuts, cand)) {
                    log("  [rewrite] module '%s': enumerated %d cut(s) with k=%d, "
                        "no more matches.\n",
                        m.name.c_str(), count_total_cuts(all_cuts), k);
                    break;
                }

                apply_rewrite(m, cand, rewrite_counter);
                total++;
            }
        }

        log("  Applied %d rewrite(s).\n", total);
    }
} MyIRRewritePass;

PRIVATE_NAMESPACE_END
