#include <algorithm>
#include <functional>
#include "bayesian/graph.hpp"
#include "bayesian/bp.hpp"
#include "bayesian/cpt.hpp" // unordered_mapのネストの解決

namespace bn {

bp::bp(graph_t const& graph)
    : graph_(graph)
{
}

void bp::initialize()
{
    pi_.clear();
    lambda_.clear();
    pi_i_.clear();
    lambda_k_.clear();
    new_pi_.clear();
    new_lambda_.clear();
    new_pi_i_.clear();
    new_lambda_k_.clear();
}

bp::return_type bp::operator()(std::unordered_map<vertex_type, matrix_type> const& precondition)
{
    initialize();

    for(auto const& node : graph_.vertex_list())
    {
        // Initialize ∀node's π (すべてのノードのπ=(1,1,...,1))
        pi_[node].resize(1, node->selectable_num, 1.0);
        
        // Initialize ∀node's λ (すべてのノードのλ=(1,1,...,1))
        lambda_[node].resize(1, node->selectable_num, 1.0);
        
        // Initialize π-message (親ノードとのメッセージの初期値)
        for(auto const& parent : graph_.in_vertexs(node))
        {
            auto& pi_i = pi_i_[node][parent];
            pi_i.resize(1, parent->selectable_num, 1.0);
        }

        // Initialize λ-message (子ノードとのメッセージの初期値)
        for(auto const& child : graph_.out_vertexs(node))
        {
            auto& lambda_k = lambda_k_[child][node];
            lambda_k.resize(1, node->selectable_num, 1.0);
        }

        // Most upstream node is evidence (最上流ノードはCPTから分かる)
        if(graph_.in_edges(node).empty())
        {
            auto& pi = pi_[node];
            auto& data = node->cpt[condition_t()].second;
            pi.resize(1, node->selectable_num);
            pi.assign(data.cbegin(), data.cend());
        }
    }
    
    // Set preconditions (事前条件の値の割り当て)
    preconditional_node_.clear();
    for(auto const& p : precondition)
    {
        preconditional_node_.push_back(p.first); // marking
        pi_[p.first] = lambda_[p.first] = p.second; // both π and λ
    }

    for(int i = 0; i < 100; ++i) // temp
    {
        // Update message (メッセージの更新)
        for(auto const& node : graph_.vertex_list())
        {
            for(auto const& parent : graph_.in_vertexs(node))
            {
                calculate_pi_i(node, parent);
            }
            for(auto const& child : graph_.out_vertexs(node))
            {
                calculate_lambda_k(child, node);
            }
        }

        // Update node (頂点の更新)
        for(auto const& node : graph_.vertex_list())
        {
            if(new_pi_.find(node) == new_pi_.cend())
            {
                calculate_pi(node);
            }
            if(new_lambda_.find(node) == new_lambda_.cend())
            {
                calculate_lambda(node);
            }
        }

        // Set future state to current state (push forward time)
        // πとλの未来の状態を現在の状態にコピーする(世代交代)
        for(auto const& outer : new_pi_) pi_[outer.first] = outer.second;
        for(auto const& outer : new_lambda_) lambda_[outer.first] = outer.second;
        for(auto const& outer : new_pi_i_) for(auto const& inner : outer.second) pi_i_[outer.first][inner.first] = inner.second;
        for(auto const& outer : new_lambda_k_) for(auto const& inner : outer.second) lambda_k_[outer.first][inner.first] = inner.second;

        new_pi_.clear();
        new_lambda_.clear();
        new_pi_i_.clear();
        new_lambda_k_.clear();
    }
        
    // すべてのπとλから，返すための値を作っていく
    return_type result;
    for(auto const& node : graph_.vertex_list())
    {
        // 上からの確率と下からの確率を掛けあわせ，正規化する
        auto raw_bel = pi_[node] % lambda_[node];
        result[node] = normalize(raw_bel);
    }
    return result;
}

matrix_type& bp::normalize(matrix_type& target) const
{
    double sum = 0;

    for(std::size_t i = 0; i < target.height(); ++i)
        for(std::size_t j = 0; j < target.width(); ++j)
            sum += target[i][j];

    for(std::size_t i = 0; i < target.height(); ++i)
        for(std::size_t j = 0; j < target.width(); ++j)
            target[i][j] /= sum;

    return target;
}

void bp::calculate_pi(vertex_type const& target)
{
    // 事前条件ノードならば更新をしない
    if(is_preconditional_node(target)) return;

    auto const in_vertexs = graph_.in_vertexs(target);

    matrix_type matrix(1, target->selectable_num, 0.0);
    all_combination_pattern(
        in_vertexs,
        [&](condition_t const& condition)
        {
            auto const& cpt_data = target->cpt[condition].second;
            for(int i = 0; i < target->selectable_num; ++i)
            {
                double value = cpt_data[i];
                for(auto const& xi : in_vertexs)
                {
                    value *= pi_i_[target][xi][0][condition.at(xi)];
                }
                matrix[0][i] += value;
            }
        });

    // 計算された値でπを更新させる
    new_pi_[target] = normalize(matrix);
}

void bp::calculate_pi_i(vertex_type const& from, vertex_type const& target)
{
    auto out_vertexs = graph_.out_vertexs(target);
    out_vertexs.erase(std::find(out_vertexs.cbegin(), out_vertexs.cend(), from));

    matrix_type matrix = pi_[target];
    for(int i = 0; i < target->selectable_num; ++i)
    {
        for(auto const& xj : out_vertexs)
        {
            matrix[0][i] *= lambda_k_[xj][target][0][i];
        }
    }

    // 計算された値でπiを更新させる
    new_pi_i_[from][target] = normalize(matrix);
}

void bp::calculate_lambda(vertex_type const& target)
{
    // 事前条件ノードならば更新をしない
    if(is_preconditional_node(target)) return;

    auto const out_vertexs = graph_.out_vertexs(target);

    matrix_type matrix(1, target->selectable_num, 1.0);
    for(int i = 0; i < target->selectable_num; ++i)
    {
        for(auto const& xi : out_vertexs)
        {
            matrix[0][i] *= lambda_k_[xi][target][0][i];
        }
    }

    // 計算された値でλを更新させる
    new_lambda_[target] = normalize(matrix);
}

void bp::calculate_lambda_k(vertex_type const& from, vertex_type const& target)
{
    auto const in_vertexs = graph_.in_vertexs(from);
    
    matrix_type matrix(1, target->selectable_num, 0.0);
    for(int i = 0; i < from->selectable_num; ++i)
    {
        auto const times = lambda_[from][0][i];
        all_combination_pattern(
            in_vertexs,
            [&](condition_t const& cond)
            {
                double value = times * from->cpt[cond].second[i];
                for(auto const& p : cond)
                {
                    if(p.first != target)
                    {
                        value *= pi_i_[from][p.first][0][p.second];
                    }
                }
                matrix[0][cond.at(target)] += value;
            });
    }

    // 計算された値でλkを更新させる
    new_lambda_k_[from][target] = normalize(matrix);
}

// 与えられた確率変数全ての組み合わせに対し，functionを実行するというインターフェースを提供する
void bp::all_combination_pattern(
    std::vector<vertex_type> const& combination,
    std::function<void(condition_t const&)> const& function
    ) const
{
    typedef std::vector<vertex_type>::const_iterator iterator_type;
    std::function<void(iterator_type const, iterator_type const&)> recursive;

    condition_t condition;
    recursive = [&](iterator_type const it, iterator_type const& end)
    {
        if(it == end)
        {
            function(condition);
        }
        else
        {
            for(int i = 0; i < (*it)->selectable_num; ++i)
            {
                condition[*it] = i;
                recursive(it + 1, end);
            }
        }
    };

    recursive(combination.cbegin(), combination.cend());
}

bool bp::is_preconditional_node(vertex_type const& node) const
{
    auto const it = std::find(preconditional_node_.cbegin(), preconditional_node_.cend(), node);
    return it != preconditional_node_.cend();
}

} // namespace bn

