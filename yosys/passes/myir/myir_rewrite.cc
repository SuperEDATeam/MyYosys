#include "kernel/yosys.h"
#include "passes/myir/myir.h"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

namespace {

struct CutInfo {
    std::set<std::string> leaves;
    std::set<int> nodes;
};

struct CutStats {
    int peak_cuts_per_node = 0;
    int truncated_nodes = 0;
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

static void combine_cut_options(const std::vector<std::vector<CutInfo>> &options,
                                int pos, int k, CutInfo current,
                                std::vector<CutInfo> &combined,
                                std::unordered_set<std::string> &seen)
{
    if (pos == (int)options.size()) {
        // Dedup via a hash set of cut keys instead of an O(n^2) linear scan;
        // on wide designs a node can produce thousands of intermediate cut
        // combinations and the quadratic check dominated runtime.
        std::string key = cut_key(current);
        if (seen.insert(key).second)
            combined.push_back(current);
        return;
    }

    for (const auto &opt : options[pos]) {
        CutInfo merged = current;
        merged.leaves.insert(opt.leaves.begin(), opt.leaves.end());
        if ((int)merged.leaves.size() > k)
            continue;

        merged.nodes.insert(opt.nodes.begin(), opt.nodes.end());
        combine_cut_options(options, pos + 1, k, merged, combined, seen);
    }
}

// Sort cuts so that the "most useful" ones come first when we have to
// truncate.  Heuristic: fewer leaves > fewer internal nodes > deterministic
// tie-break on string key.
static void rank_and_truncate(std::vector<CutInfo> &cuts,
                              int max_cuts_per_node,
                              CutStats &stats)
{
    if (max_cuts_per_node <= 0)
        return;                          // 0 = unlimited
    if ((int)cuts.size() <= max_cuts_per_node)
        return;

    std::sort(cuts.begin(), cuts.end(),
              [](const CutInfo &a, const CutInfo &b) {
                  if (a.leaves.size() != b.leaves.size())
                      return a.leaves.size() < b.leaves.size();
                  if (a.nodes.size() != b.nodes.size())
                      return a.nodes.size() < b.nodes.size();
                  return cut_key(a) < cut_key(b);
              });

    cuts.resize(max_cuts_per_node);
    stats.truncated_nodes++;
}

static std::vector<CutInfo> enumerate_cuts_rec(
    int idx, const MyIR::Module &m, int k, int max_cuts_per_node,
    const std::unordered_map<std::string, int> &producer,
    std::map<int, std::vector<CutInfo>> &memo,
    CutStats &stats)
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
                enumerate_cuts_rec(prod_it->second, m, k, max_cuts_per_node,
                                   producer, memo, stats);
            opts.insert(opts.end(), child_cuts.begin(), child_cuts.end());
        }

        operand_options.push_back(opts);
    }

    std::vector<CutInfo> cuts;
    std::unordered_set<std::string> seen;
    combine_cut_options(operand_options, 0, k, CutInfo(), cuts, seen);

    for (auto &cut : cuts)
        cut.nodes.insert(idx);

    rank_and_truncate(cuts, max_cuts_per_node, stats);

    if ((int)cuts.size() > stats.peak_cuts_per_node)
        stats.peak_cuts_per_node = (int)cuts.size();

    memo[idx] = cuts;
    return cuts;
}

static std::map<int, std::vector<CutInfo>> enumerate_all_cuts(
    const MyIR::Module &m, int k, int max_cuts_per_node,
    const std::unordered_map<std::string, int> &producer,
    CutStats &stats)
{
    std::map<int, std::vector<CutInfo>> memo;
    for (int i = 0; i < (int)m.exprs.size(); i++)
        enumerate_cuts_rec(i, m, k, max_cuts_per_node, producer, memo, stats);
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

// Collect the single best matching rewrite for *each* root expression (a
// given root can host at most one rewrite, so we keep its highest-scoring
// match across all of its cuts).  Returned unsorted; the caller ranks and
// greedily picks a non-overlapping batch.
static void find_all_rewrite_candidates(
    const MyIR::Module &m, int k,
    const std::unordered_map<std::string, int> &producer,
    const std::unordered_map<std::string, int> &uses,
    const std::map<int, std::vector<CutInfo>> &all_cuts,
    std::vector<RewriteCandidate> &out)
{
    for (int idx = 0; idx < (int)m.exprs.size(); idx++)
    {
        auto cut_it = all_cuts.find(idx);
        if (cut_it == all_cuts.end())
            continue;

        bool found = false;
        RewriteCandidate best;

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

        if (found)
            out.push_back(best);
    }
}

// All expression indices a candidate reads or rewrites.  Two candidates whose
// touched sets are disjoint can be applied together in one rebuild: rewrite
// rules only delete intermediate nodes whose use-count is exactly 1 (their
// sole user lives inside the same pattern), while leaf wires and the root
// result wire are always preserved, so disjoint index sets cannot interfere.
static std::set<int> touched_indices(const RewriteCandidate &cand)
{
    if (cand.kind == RewriteKind::Factor)
        return { cand.root_idx, cand.lhs_idx, cand.rhs_idx };
    return cand.remove_nodes;   // redirect: root + nested nodes already included
}

// A deferred batch of rewrites: drop `remove` indices, swap `replace` indices
// for their new expr list, and append `connects`.  Building a plan instead of
// mutating m.exprs immediately lets us apply many non-overlapping rewrites in
// a single rebuild (one O(E) pass) rather than re-enumerating every cut after
// each individual rewrite.
struct RewritePlan {
    std::set<int> remove;
    std::map<int, std::vector<MyIR::Expr>> replace;
    std::vector<MyIR::Assign> connects;
};

static void plan_redirect_rewrite(MyIR::Module &m, const RewriteCandidate &cand,
                                  RewritePlan &plan)
{
    const MyIR::Expr &root = m.exprs[cand.root_idx];

    for (int i : cand.remove_nodes)
        plan.remove.insert(i);

    if (root.result_wire != cand.redirect_target) {
        MyIR::Assign a;
        a.lhs = root.result_wire;
        a.rhs = cand.redirect_target;
        plan.connects.push_back(a);
    }
}

static void plan_factor_rewrite(MyIR::Module &m, const RewriteCandidate &cand,
                                int &rewrite_counter, RewritePlan &plan)
{
    const MyIR::Expr old_root = m.exprs[cand.root_idx];

    std::string temp = fresh_wire_name(m, rewrite_counter);
    MyIR::Port w;
    w.name = temp;
    w.dir = MyIR::PortDir::Internal;
    w.width = old_root.result_width;
    m.wires[temp] = w;

    MyIR::Expr inner;
    inner.result_wire = temp;
    inner.result_width = old_root.result_width;
    inner.operands.push_back(cand.lhs_other);
    inner.operands.push_back(cand.rhs_other);

    MyIR::Expr new_root;
    new_root.result_wire = old_root.result_wire;
    new_root.result_width = old_root.result_width;

    if (cand.rule_name == "demorgan_or" || cand.rule_name == "demorgan_and") {
        inner.op = old_root.op == MyIR::OpType::Or ? MyIR::OpType::And
                                                   : MyIR::OpType::Or;
        new_root.op = MyIR::OpType::Not;
        new_root.operands.push_back(temp);
    } else if (cand.rule_name == "resub_or" || cand.rule_name == "resub_and") {
        inner.op = cand.rule_name == "resub_or" ? MyIR::OpType::Or
                                                : MyIR::OpType::And;
        new_root.op = MyIR::OpType::Buf;
        new_root.operands.push_back(temp);
    } else {
        // distributive factor: parent.op stays, inner takes the parent op
        const MyIR::Expr &old_lhs = m.exprs[cand.lhs_idx];
        inner.op = old_root.op;
        new_root = old_root;
        new_root.op = old_lhs.op;
        new_root.operands.clear();
        new_root.operands.push_back(cand.common);
        new_root.operands.push_back(temp);
    }

    plan.remove.insert(cand.lhs_idx);
    plan.remove.insert(cand.rhs_idx);
    plan.replace[cand.root_idx] = { inner, new_root };
}

static void plan_rewrite(MyIR::Module &m, const RewriteCandidate &cand,
                         int &rewrite_counter, RewritePlan &plan)
{
    if (cand.kind == RewriteKind::Factor)
        plan_factor_rewrite(m, cand, rewrite_counter, plan);
    else
        plan_redirect_rewrite(m, cand, plan);
}

// Rebuild m.exprs once, applying every queued remove/replace, then append the
// queued connects.  `replace` always takes precedence over `remove` for the
// same index (a factor root is replaced, not dropped); non-overlapping
// candidate selection guarantees an index never appears in both for distinct
// candidates.
static void apply_plan(MyIR::Module &m, const RewritePlan &plan)
{
    if (plan.remove.empty() && plan.replace.empty() && plan.connects.empty())
        return;

    std::vector<MyIR::Expr> new_exprs;
    new_exprs.reserve(m.exprs.size() + plan.replace.size());
    for (int i = 0; i < (int)m.exprs.size(); i++) {
        auto rit = plan.replace.find(i);
        if (rit != plan.replace.end()) {
            for (const auto &e : rit->second)
                new_exprs.push_back(e);
            continue;
        }
        if (plan.remove.count(i))
            continue;
        new_exprs.push_back(m.exprs[i]);
    }
    m.exprs.swap(new_exprs);

    for (const auto &a : plan.connects)
        m.connects.push_back(a);
}

} // namespace

struct MyIRRewritePass : public Pass {
    MyIRRewritePass() : Pass("myir_rewrite",
        "k-cut based algebraic rewriting on MyIR") {}

    void help() override
    {
        log("\n");
        log("    myir_rewrite [-k N] [-max-cuts N] [-stats]\n");
        log("\n");
        log("Enumerate k-cuts for each MyIR expression and run local\n");
        log("pattern-based rewriting on the cuts.\n");
        log("\n");
        log("Options:\n");
        log("    -k N\n");
        log("        Maximum number of distinct leaves allowed in a cut.\n");
        log("        Default: 4.\n");
        log("\n");
        log("    -max-cuts N\n");
        log("        Cap the number of cuts kept per node at N.  Excess\n");
        log("        cuts are dropped after ranking by (fewer leaves first,\n");
        log("        then fewer internal nodes).  0 = unlimited.\n");
        log("        Default: 16.\n");
        log("\n");
        log("    -stats\n");
        log("        Also print per-rule hit counts in the summary log.\n");
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
        log("The pass precomputes cuts up to k leaves and scores all matching\n");
        log("local rewrites.  Each round it ranks candidates by benefit and\n");
        log("greedily applies a maximal batch whose touched nodes are pairwise\n");
        log("disjoint, then re-enumerates and repeats until no profitable rule\n");
        log("matches remain.  Batching keeps the number of full cut\n");
        log("re-enumerations proportional to the number of rounds rather than\n");
        log("the number of individual rewrites, which is what makes\n");
        log("multi-thousand-cell designs tractable.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing myir_rewrite.\n");

        int k = 4;
        int max_cuts_per_node = 16;
        bool flag_stats = false;
        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++)
        {
            if (args[argidx] == "-k" && argidx + 1 < args.size()) {
                k = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-max-cuts" && argidx + 1 < args.size()) {
                max_cuts_per_node = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-stats") {
                flag_stats = true;
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        if (k < 2)
            log_error("myir_rewrite: -k must be >= 2.\n");
        if (max_cuts_per_node < 0)
            log_error("myir_rewrite: -max-cuts must be >= 0 (0 = unlimited).\n");

        MyIR::Design *myir = MyIR::get_design();
        if (!myir || myir->modules.empty()) {
            log_warning("No MyIR design loaded.\n");
            return;
        }

        int total = 0;
        int total_iterations = 0;
        long long total_cuts_enumerated = 0;
        int max_peak_cuts = 0;
        int total_truncated_nodes = 0;
        std::map<std::string, int> rule_hits;

        for (auto &mkv : myir->modules)
        {
            MyIR::Module &m = mkv.second;
            int rewrite_counter = 0;

            while (true) {
                total_iterations++;
                auto producer = build_producer_map(m);
                auto uses = build_use_count(m);
                CutStats stats;
                auto all_cuts = enumerate_all_cuts(m, k, max_cuts_per_node,
                                                   producer, stats);
                int this_cuts = count_total_cuts(all_cuts);
                total_cuts_enumerated += this_cuts;
                if (stats.peak_cuts_per_node > max_peak_cuts)
                    max_peak_cuts = stats.peak_cuts_per_node;
                total_truncated_nodes += stats.truncated_nodes;

                std::vector<RewriteCandidate> cands;
                find_all_rewrite_candidates(m, k, producer, uses, all_cuts, cands);

                if (cands.empty()) {
                    log("  [rewrite] module '%s': enumerated %d cut(s) with k=%d, "
                        "no more matches.\n",
                        m.name.c_str(), this_cuts, k);
                    break;
                }

                // Highest-benefit first, then greedily pick a maximal set of
                // candidates whose touched node-index sets are pairwise
                // disjoint, and apply them all in one rebuild.  This collapses
                // the number of full cut re-enumerations from O(#rewrites) to
                // O(#rounds), which is what made multi-thousand-cell designs
                // intractable before.
                std::sort(cands.begin(), cands.end(),
                          [](const RewriteCandidate &a, const RewriteCandidate &b) {
                              return a.score > b.score;
                          });

                RewritePlan plan;
                std::set<int> used;
                int applied = 0;
                for (const auto &cand : cands) {
                    std::set<int> touched = touched_indices(cand);
                    bool conflict = false;
                    for (int x : touched)
                        if (used.count(x)) { conflict = true; break; }
                    if (conflict)
                        continue;

                    used.insert(touched.begin(), touched.end());
                    plan_rewrite(m, cand, rewrite_counter, plan);
                    rule_hits[cand.rule_name]++;
                    total++;
                    applied++;
                }

                apply_plan(m, plan);

                if (applied == 0)
                    break;       // safety: no progress this round
            }
        }

        log("  myir_rewrite summary:\n");
        log("    iterations            : %d\n", total_iterations);
        log("    total cuts enumerated : %lld (peak per node = %d)\n",
            total_cuts_enumerated, max_peak_cuts);
        if (total_truncated_nodes > 0)
            log("    cuts truncated by -max-cuts : %d node(s) hit the cap\n",
                total_truncated_nodes);
        log("    rewrites applied      : %d\n", total);
        if (flag_stats && !rule_hits.empty()) {
            log("    by rule:\n");
            for (const auto &kv : rule_hits)
                log("      %-22s : %d\n", kv.first.c_str(), kv.second);
        }
    }
} MyIRRewritePass;

PRIVATE_NAMESPACE_END
