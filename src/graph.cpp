#include "dag/core.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace dag {
namespace {
std::size_t add(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        throw std::runtime_error("graph size overflow");
    return a + b;
}
std::size_t mul(std::size_t a, std::size_t b) {
    if (a && b > std::numeric_limits<std::size_t>::max() / a)
        throw std::runtime_error("graph size overflow");
    return a * b;
}
std::size_t task_bytes(const std::vector<Task> &tasks) {
    std::size_t n = mul(tasks.capacity(), sizeof(Task));
    for (const auto &t : tasks) {
        n = add(n, t.symbol.capacity() + 1);
        n = add(n, mul(t.args.capacity(), sizeof(Int)));
        for (const auto *rs : {&t.reads, &t.writes}) {
            n = add(n, mul(rs->capacity(), sizeof(Region)));
            for (const auto &r : *rs)
                n = add(n, r.buffer.capacity() + 1);
        }
    }
    return n;
}
void check_region(const Region &r) {
    if (r.buffer.empty())
        throw std::runtime_error("empty access buffer");
    if (!r.rows || !r.cols)
        return;
    if (r.rows > 1 && r.stride < r.cols)
        throw std::runtime_error("overlapping rows in access region");
    (void)add(add(r.offset, mul(r.rows - 1, r.stride)), r.cols);
}
bool overlap(const Region &a, const Region &b) {
    if (a.buffer != b.buffer || !a.rows || !a.cols || !b.rows || !b.cols)
        return false;
    std::size_t i = 0, j = 0;
    while (i < a.rows && j < b.rows) {
        const auto x = a.offset + i * a.stride, y = b.offset + j * b.stride;
        if (x < y + b.cols && y < x + a.cols)
            return true;
        if (x + a.cols <= y)
            ++i;
        else
            ++j;
    }
    return false;
}
bool intersects(const std::vector<Region> &a, const std::vector<Region> &b) {
    for (const auto &x : a)
        for (const auto &y : b)
            if (overlap(x, y))
                return true;
    return false;
}
bool conflict(const Task &a, const Task &b) {
    return intersects(a.writes, b.reads) || intersects(a.reads, b.writes) ||
           intersects(a.writes, b.writes);
}
struct Segment {
    std::size_t end;
    std::uint32_t writer = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> readers;
};
struct Budget {
    std::size_t used, limit;
    void take(std::size_t bytes) {
        if (bytes > limit || used > limit - bytes)
            throw ResourceLimit("graph memory budget exceeded");
        used += bytes;
    }
};
template <class T> void grow(std::vector<T> &v, std::size_t needed, Budget &b) {
    if (needed <= v.capacity())
        return;
    auto cap = std::max(needed, v.capacity() ? mul(v.capacity(), 2) : std::size_t(8));
    // During reserve the old allocation can coexist with the new allocation.
    b.take(mul(cap, sizeof(T)));
    v.reserve(cap);
    // Keep old allocation charge conservatively: graph construction has a hard envelope.
}
} // namespace

Graph build_graph(std::vector<Task> tasks, const Limits &limits) {
    if (limits.max_tasks && tasks.size() > limits.max_tasks)
        throw std::runtime_error("task count budget exceeded");
    if (tasks.size() >= std::numeric_limits<std::uint32_t>::max())
        throw ResourceLimit("task count exceeds graph capacity");
    Budget budget{0, limits.memory_bytes};
    budget.take(task_bytes(tasks));
    for (const auto &t : tasks)
        for (const auto *rr : {&t.reads, &t.writes})
            for (const auto &r : *rr)
                check_region(r);
    // Partition physical cells by their complete declared-access membership.
    // Disjoint intervals with the same membership can share dependency state:
    // no task in this execution can access one without accessing the others.
    // This is generic access compression, not an algorithm-specific tile rule.
    struct RegionLess {
        bool operator()(const Region &a, const Region &b) const {
            return std::tie(a.buffer, a.offset, a.rows, a.cols, a.stride) <
                   std::tie(b.buffer, b.offset, b.rows, b.cols, b.stride);
        }
    };
    std::map<Region, std::size_t, RegionLess> region_ids;
    std::vector<const Region *> regions;
    std::map<std::string, std::size_t> buffer_ids;
    struct Event {
        std::size_t buffer, position, region;
        bool start;
    };
    std::vector<Event> events;
    std::size_t steps = 0;
    auto tick = [&] {
        if (steps == std::numeric_limits<std::size_t>::max() ||
            (limits.max_steps && steps >= limits.max_steps))
            throw std::runtime_error("dependency construction work budget exceeded");
        ++steps;
    };
    for (const auto &t : tasks)
        for (const auto *accesses : {&t.reads, &t.writes})
            for (const auto &r : *accesses) {
                tick();
                if (!r.rows || !r.cols)
                    continue;
                if (region_ids.find(r) != region_ids.end())
                    continue;
                budget.take(sizeof(std::pair<const Region, std::size_t>) + r.buffer.capacity() +
                            128);
                auto inserted = region_ids.emplace(r, region_ids.size()).first;
                grow(regions, regions.size() + 1, budget);
                regions.push_back(&inserted->first);
                auto bit = buffer_ids.find(r.buffer);
                if (bit == buffer_ids.end()) {
                    budget.take(sizeof(std::pair<const std::string, std::size_t>) +
                                r.buffer.capacity() + 128);
                    bit = buffer_ids.emplace(r.buffer, buffer_ids.size()).first;
                }
                for (std::size_t row = 0; row < r.rows; ++row) {
                    tick();
                    const auto begin = r.offset + row * r.stride;
                    grow(events, add(events.size(), 2), budget);
                    events.push_back({bit->second, begin, inserted->second, true});
                    events.push_back({bit->second, begin + r.cols, inserted->second, false});
                }
            }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        return std::tie(a.buffer, a.position, a.start, a.region) <
               std::tie(b.buffer, b.position, b.start, b.region);
    });
    budget.take(mul(regions.size(), sizeof(std::vector<std::size_t>)));
    std::vector<std::vector<std::size_t>> memberships(regions.size());
    std::map<std::vector<std::size_t>, std::size_t> signatures;
    budget.take(mul(regions.size(), sizeof(std::size_t) + 4 * sizeof(void *) + 32));
    std::set<std::size_t> active;
    std::vector<std::size_t> signature;
    std::vector<Segment> states;
    for (std::size_t i = 0; i < events.size();) {
        const auto buffer = events[i].buffer, position = events[i].position;
        std::size_t j = i;
        while (j < events.size() && events[j].buffer == buffer && events[j].position == position) {
            tick();
            const auto &e = events[j++];
            if (e.start) {
                // The active set never exceeds the number of unique regions.
                if (!active.insert(e.region).second)
                    throw std::runtime_error("overlapping access-region rows");
            } else if (active.erase(e.region) != 1)
                throw std::runtime_error("invalid region interval events");
        }
        if (j < events.size() && events[j].buffer == buffer && !active.empty()) {
            signature.clear();
            grow(signature, active.size(), budget);
            for (auto rid : active) {
                tick();
                signature.push_back(rid);
            }
            if (signatures.find(signature) == signatures.end()) {
                const auto state = states.size();
                budget.take(sizeof(std::pair<const std::vector<std::size_t>, std::size_t>) +
                            mul(signature.size(), sizeof(std::size_t)) + 128);
                signatures.emplace(signature, state);
                grow(states, state + 1, budget);
                states.push_back(Segment{0, std::numeric_limits<std::uint32_t>::max(), {}});
                for (auto rid : signature) {
                    grow(memberships[rid], memberships[rid].size() + 1, budget);
                    memberships[rid].push_back(state);
                }
            }
        }
        if (j == events.size() || events[j].buffer != buffer) {
            if (!active.empty())
                throw std::runtime_error("unclosed region interval");
        }
        i = j;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    std::vector<std::uint32_t> deps;
    for (std::uint32_t id = 0; id < tasks.size(); ++id) {
        deps.clear();
        auto dependency = [&](std::uint32_t src) {
            if (src == id || src == std::numeric_limits<std::uint32_t>::max())
                return;
            grow(deps, deps.size() + 1, budget);
            deps.push_back(src);
        };
        auto access = [&](const Region &r, bool write) {
            if (!r.rows || !r.cols)
                return;
            const auto rid = region_ids.find(r)->second;
            for (auto sid : memberships[rid]) {
                tick();
                auto &state = states[sid];
                dependency(state.writer);
                if (write) {
                    for (auto reader : state.readers)
                        dependency(reader);
                    state.readers.clear();
                    state.writer = id;
                } else if (state.readers.empty() || state.readers.back() != id) {
                    grow(state.readers, state.readers.size() + 1, budget);
                    state.readers.push_back(id);
                }
            }
        };
        for (const auto &r : tasks[id].reads)
            access(r, false);
        for (const auto &r : tasks[id].writes)
            access(r, true);
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        grow(edges, add(edges.size(), deps.size()), budget);
        for (auto src : deps)
            edges.emplace_back(src, id);
    }
    std::sort(edges.begin(), edges.end());
    const auto n = tasks.size();
    budget.take(mul(n + 1, sizeof(std::size_t)));
    budget.take(mul(n, sizeof(std::uint32_t)));
    budget.take(mul(edges.size(), sizeof(std::uint32_t)));
    Graph graph;
    graph.offsets.assign(n + 1, 0);
    graph.indegrees.assign(n, 0);
    graph.successors.resize(edges.size());
    for (std::size_t i = 0; i < edges.size(); ++i) {
        auto [src, dst] = edges[i];
        if (graph.indegrees[dst] == std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("dependency count overflow");
        ++graph.offsets[src + 1];
        ++graph.indegrees[dst];
        graph.successors[i] = dst;
    }
    for (std::size_t i = 1; i < graph.offsets.size(); ++i)
        graph.offsets[i] += graph.offsets[i - 1];
    graph.tasks = std::move(tasks);
    graph.memory_bytes = add(task_bytes(graph.tasks),
                             add(mul(graph.offsets.capacity(), sizeof(std::size_t)),
                                 add(mul(graph.indegrees.capacity(), sizeof(std::uint32_t)),
                                     mul(graph.successors.capacity(), sizeof(std::uint32_t)))));
    return graph;
}

void validate_graph(const Graph &g) {
    const auto n = g.tasks.size();
    if (n > 4096)
        throw std::runtime_error("independent graph audit limited to 4096 tasks");
    if (g.offsets.size() != n + 1 || g.indegrees.size() != n || g.offsets.front() != 0 ||
        g.offsets.back() != g.successors.size())
        throw std::runtime_error("invalid CSR graph");
    // Validate every range before traversing any adjacency, including future offsets.
    for (std::size_t i = 0; i < g.offsets.size(); ++i)
        if (g.offsets[i] > g.successors.size() || (i && g.offsets[i] < g.offsets[i - 1]))
            throw std::runtime_error("invalid CSR offset range");
    for (const auto &t : g.tasks)
        for (const auto *rs : {&t.reads, &t.writes})
            for (const auto &r : *rs)
                check_region(r);
    const auto words = (n + 63) / 64;
    std::vector<std::vector<std::uint64_t>> actual(n, std::vector<std::uint64_t>(words)),
        reference = actual;
    std::vector<std::uint32_t> counts(n, 0);
    for (std::size_t src = 0; src < n; ++src) {
        if (g.offsets[src] > g.offsets[src + 1])
            throw std::runtime_error("invalid CSR offsets");
        std::uint32_t previous = 0;
        bool first = true;
        for (auto e = g.offsets[src]; e < g.offsets[src + 1]; ++e) {
            auto dst = g.successors[e];
            if (dst <= src || dst >= n || (!first && dst <= previous))
                throw std::runtime_error("non-forward or duplicate graph edge");
            previous = dst;
            first = false;
            ++counts[dst];
            actual[src][dst / 64] |= std::uint64_t(1) << (dst % 64);
        }
        for (std::size_t dst = src + 1; dst < n; ++dst)
            if (conflict(g.tasks[src], g.tasks[dst]))
                reference[src][dst / 64] |= std::uint64_t(1) << (dst % 64);
    }
    if (counts != g.indegrees)
        throw std::runtime_error("indegree/edge mismatch");
    for (auto *matrix : {&actual, &reference})
        for (std::size_t i = n; i-- > 0;)
            for (std::size_t j = i + 1; j < n; ++j)
                if ((*matrix)[i][j / 64] & (std::uint64_t(1) << (j % 64)))
                    for (std::size_t k = 0; k < words; ++k)
                        (*matrix)[i][k] |= (*matrix)[j][k];
    if (actual != reference)
        throw std::runtime_error("graph does not preserve exact conflict reachability");
}
} // namespace dag
