// Offline export of the real bound graph for the small HTML demonstration.
#include "dag/core.hpp"
#include <iostream>
#include <limits>
#include <string>

namespace {
void quoted(const std::string &text) {
    std::cout << '"';
    for (char c : text) {
        if (c == '"' || c == '\\') std::cout << '\\';
        std::cout << c;
    }
    std::cout << '"';
}
void regions(const std::vector<dag::Region> &items) {
    std::cout << '[';
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) std::cout << ',';
        const auto &r = items[i];
        std::cout << "{\"buffer\":";
        quoted(r.buffer);
        std::cout << ",\"offset\":" << r.offset << ",\"rows\":" << r.rows
                  << ",\"cols\":" << r.cols << ",\"stride\":" << r.stride << '}';
    }
    std::cout << ']';
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: dag_demo_export metadata n b");
        const auto n = std::stoll(argv[2]), b = std::stoll(argv[3]);
        // Display limit only; this does not restrict the production runtime.
        if (n < 3 || n > 16 || b < 1 || n % b || n / b > 4)
            throw std::runtime_error("visual demo requires 3<=n<=16, n%b=0, n/b<=4");
        dag::Limits limits;
        limits.loop_chunk = 1;
        limits.loop_parts = 0;
        const auto program = dag::load_metadata(argv[1], limits);
        auto tasks = dag::instantiate(program, {n, b}, limits);
        const auto semantic_count = tasks.size();
        if (!tasks.empty() && tasks.front().symbol == "__copy") tasks.erase(tasks.begin());
        if (!tasks.empty() && tasks.back().symbol == "__zero_upper") tasks.pop_back();
        const auto graph = dag::build_graph(std::move(tasks), limits);
        std::cout << "{\"semantic_task_count\":" << semantic_count << ",\"tasks\":[";
        for (std::size_t i = 0; i < graph.tasks.size(); ++i) {
            if (i) std::cout << ',';
            const auto &t = graph.tasks[i];
            std::cout << "{\"id\":" << i << ",\"symbol\":";
            quoted(t.symbol);
            std::cout << ",\"args\":[";
            for (std::size_t j = 0; j < t.args.size(); ++j) {
                if (j) std::cout << ',';
                std::cout << t.args[j];
            }
            std::cout << "],\"reads\":";
            regions(t.reads);
            std::cout << ",\"writes\":";
            regions(t.writes);
            std::cout << ",\"indegree\":" << graph.indegrees[i] << '}';
        }
        std::cout << "],\"edges\":[";
        bool first = true;
        for (std::size_t src = 0; src < graph.tasks.size(); ++src)
            for (auto e = graph.offsets[src]; e < graph.offsets[src + 1]; ++e) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << '[' << src << ',' << graph.successors[e] << ']';
            }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "demo export failed: " << e.what() << '\n';
        return 1;
    }
}
