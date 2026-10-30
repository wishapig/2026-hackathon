#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dag {
using Int = std::int64_t;
// Expressions are prefix trees, with literal/variable leaves.
struct Expr {
    std::string op;
    Int value = 0;
    std::string name;
    std::vector<Expr> args;
};
struct RegionExpr {
    std::string buffer;
    Expr offset, rows, cols, stride;
};
struct Node {
    enum class Kind { Let, For, If, Call, Copy, ZeroUpper } kind;
    std::string name, symbol;
    Expr value, start, stop, step, condition;
    std::vector<Expr> args;
    std::vector<RegionExpr> reads, writes;
    std::vector<Node> body, otherwise;
};
struct Program {
    std::string source_digest, contract_digest;
    std::vector<std::string> operators;
    std::vector<Node> nodes;
    std::vector<Expr> parameter_guards;
    std::size_t artifact_bytes = 0;
    std::size_t retained_bytes = 0;
    bool source_int32 = false;
};
struct Region {
    std::string buffer;
    std::size_t offset = 0, rows = 0, cols = 0, stride = 0;
};
struct Task {
    std::string symbol;
    std::vector<Int> args;
    std::vector<Region> reads, writes;
};
struct Parameters {
    Int n = 0, b = 0;
};
struct Limits {
    std::size_t memory_bytes = 512ull * 1024 * 1024;
    // Zero means no user-imposed total-work limit; arithmetic remains checked.
    std::size_t max_tasks = 0;
    std::size_t max_steps = 0;
};
// Resource exhaustion during preparation may select a streaming path before
// any numerical operation. Invalid metadata and execution failures never do.
struct ResourceLimit : std::runtime_error {
    using std::runtime_error::runtime_error;
};
Program load_metadata(const std::string &path, const Limits &limits = {});
void enumerate_program(const Program &, Parameters, const Limits &,
                       const std::function<void(const Task &)> &);
std::vector<Task> instantiate(const Program &, Parameters, const Limits &);

struct Graph {
    std::vector<Task> tasks;
    std::vector<std::size_t> offsets;
    std::vector<std::uint32_t> successors;
    std::vector<std::uint32_t> indegrees;
    std::size_t memory_bytes = 0;
};
Graph build_graph(std::vector<Task> tasks, const Limits &);
void validate_graph(const Graph &); // independent full-conflict reachability oracle, small graphs
struct RuntimeOptions {
    unsigned threads = 0;
    unsigned batch = 1;
    std::string backend = "atomic";
    bool pin = true;
    bool audit = false;
    std::size_t memory_bytes = 512ull * 1024 * 1024;
};
struct ExecutionStats {
    std::uint64_t tasks = 0, edges = 0, steals = 0;
    unsigned workers = 0;
    double execute_seconds = 0;
};
std::vector<unsigned> allowed_node_cpus();
// Reusable workers; constructor/destructor costs belong to the caller's timing.
class Executor {
  public:
    static std::size_t minimum_reserved_bytes(unsigned workers);
    explicit Executor(RuntimeOptions);
    ~Executor();
    Executor(const Executor &) = delete;
    Executor &operator=(const Executor &) = delete;
    ExecutionStats run(const Graph &, const std::function<void(const Task &)> &,
                       const std::function<void()> &before = {});
    void set_memory_budget(std::size_t bytes);
    std::size_t reserved_bytes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dag
