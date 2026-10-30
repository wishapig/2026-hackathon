#include "dag/core.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace dag {
namespace {
constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
// Explicit worker stacks make the scheduler's resource reservation predictable.
constexpr std::size_t worker_stack_bytes = 1024 * 1024;
std::size_t checked_add(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        throw std::runtime_error("runtime memory size overflow");
    return a + b;
}
std::size_t checked_mul(std::size_t a, std::size_t b) {
    if (a && b > std::numeric_limits<std::size_t>::max() / a)
        throw std::runtime_error("runtime memory size overflow");
    return a * b;
}
std::set<unsigned> parse_cpus(const std::string &line) {
    std::set<unsigned> result;
    std::istringstream s(line);
    std::string part;
    while (std::getline(s, part, ',')) {
        const auto dash = part.find('-');
        const auto low = std::stoul(part.substr(0, dash));
        const auto high = dash == std::string::npos ? low : std::stoul(part.substr(dash + 1));
        if (low > high || high > 1000000)
            throw std::runtime_error("invalid NUMA CPU list");
        for (unsigned long i = low; i <= high; ++i)
            result.insert(static_cast<unsigned>(i));
    }
    return result;
}
std::vector<unsigned> affinity_cpus() {
    auto n = std::max<long>(128, sysconf(_SC_NPROCESSORS_CONF));
    for (unsigned attempt = 0; attempt < 8; ++attempt, n *= 2) {
        const auto bytes = CPU_ALLOC_SIZE(n);
        cpu_set_t *set = CPU_ALLOC(n);
        if (!set)
            throw std::bad_alloc();
        CPU_ZERO_S(bytes, set);
        const int rc = sched_getaffinity(0, bytes, set), err = errno;
        if (rc == 0) {
            std::vector<unsigned> v;
            for (long i = 0; i < n; ++i)
                if (CPU_ISSET_S(i, bytes, set))
                    v.push_back(static_cast<unsigned>(i));
            CPU_FREE(set);
            if (v.empty())
                throw std::runtime_error("empty allowed CPU set");
            return v;
        }
        CPU_FREE(set);
        if (err != EINVAL)
            throw std::runtime_error("sched_getaffinity failed");
    }
    throw std::runtime_error("CPU affinity mask exceeds supported capacity");
}
void pin_cpu(unsigned cpu) {
    const auto n = std::size_t(cpu) + 1, bytes = CPU_ALLOC_SIZE(n);
    auto *set = CPU_ALLOC(n);
    if (!set)
        throw std::bad_alloc();
    CPU_ZERO_S(bytes, set);
    CPU_SET_S(cpu, bytes, set);
    const int result = pthread_setaffinity_np(pthread_self(), bytes, set);
    CPU_FREE(set);
    if (result)
        throw std::runtime_error(std::string("worker affinity failed: ") + std::strerror(result));
}
void bind_node(const std::vector<unsigned> &cpus) {
    const auto n = std::size_t(*std::max_element(cpus.begin(), cpus.end())) + 1;
    const auto bytes = CPU_ALLOC_SIZE(n);
    auto *set = CPU_ALLOC(n);
    if (!set)
        throw std::bad_alloc();
    CPU_ZERO_S(bytes, set);
    for (auto cpu : cpus)
        CPU_SET_S(cpu, bytes, set);
    const int result = pthread_setaffinity_np(pthread_self(), bytes, set);
    CPU_FREE(set);
    if (result)
        throw std::runtime_error(std::string("worker NUMA affinity failed: ") +
                                 std::strerror(result));
}
struct alignas(64) Queue {
    std::mutex mutex;
    std::size_t head = none, tail = none, count = 0;
};
} // namespace

std::vector<unsigned> allowed_node_cpus() {
    const auto allowed = affinity_cpus();
    std::vector<std::pair<unsigned, std::filesystem::path>> nodes;
    std::error_code ec;
    const std::filesystem::path root("/sys/devices/system/node");
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.size() > 4 && name.rfind("node", 0) == 0 &&
            name.find_first_not_of("0123456789", 4) == std::string::npos)
            nodes.emplace_back(static_cast<unsigned>(std::stoul(name.substr(4))), it->path());
    }
    std::sort(nodes.begin(), nodes.end());
    for (const auto &node : nodes) {
        std::ifstream f(node.second / "cpulist");
        std::string line;
        if (!std::getline(f, line))
            continue;
        auto cpus = parse_cpus(line);
        std::vector<unsigned> intersection;
        for (auto cpu : allowed)
            if (cpus.count(cpu))
                intersection.push_back(cpu);
        if (!intersection.empty())
            return intersection;
    }
    // Without topology evidence a single permitted CPU is the safe node-local fallback.
    return {allowed.front()};
}

struct Executor::Impl {
    RuntimeOptions options;
    std::vector<unsigned> cpus;
    unsigned workers = 0;
    std::vector<pthread_t> threads;
    struct ThreadArg {
        Impl *self;
        unsigned id;
    };
    std::vector<ThreadArg> arguments;
    std::unique_ptr<Queue[]> ready, inbox;
    std::vector<std::size_t> next, prev, event_next;
    std::unique_ptr<std::atomic<std::uint32_t>[]> remaining;
    std::unique_ptr<std::atomic<unsigned char>[]> seen;
    std::size_t remaining_count = 0, seen_count = 0;
    const Graph *graph = nullptr;
    const std::function<void(const Task &)> *invoke = nullptr;
    std::atomic<std::size_t> completed{0};
    std::atomic<std::uint64_t> steals{0}, wake_sequence{0};
    std::atomic<bool> failed{false};
    std::mutex control, error_mutex, wake_mutex;
    mutable std::mutex run_mutex;
    std::condition_variable start_cv, done_cv, wake_cv;
    std::exception_ptr error, startup_error;
    std::size_t epoch = 0;
    unsigned active = 0, started = 0;
    bool stopping = false;

    explicit Impl(RuntimeOptions o) : options(std::move(o)), cpus(allowed_node_cpus()) {
        if (options.backend != "atomic" && options.backend != "owner")
            throw std::runtime_error("backend must be atomic or owner");
        if (options.batch == 0 || options.batch > 4096)
            throw std::runtime_error("batch must be in [1,4096]");
        workers = options.threads ? options.threads : static_cast<unsigned>(cpus.size());
        if (!workers || workers > cpus.size())
            throw std::runtime_error("worker count exceeds allowed single-NUMA CPU set");
        const auto base = checked_mul(workers, worker_stack_bytes + 2 * sizeof(Queue) + 4096);
        if (base > options.memory_bytes)
            throw ResourceLimit("worker stacks exceed runtime memory budget");
        ready = std::make_unique<Queue[]>(workers);
        inbox = std::make_unique<Queue[]>(workers);
        arguments.resize(workers);
        threads.reserve(workers);
        pthread_attr_t attr;
        int rc = pthread_attr_init(&attr);
        if (rc)
            throw std::runtime_error("pthread_attr_init failed");
        rc = pthread_attr_setstacksize(&attr, worker_stack_bytes);
        if (rc) {
            pthread_attr_destroy(&attr);
            throw std::runtime_error("pthread stack configuration failed");
        }
        try {
            for (unsigned i = 0; i < workers; ++i) {
                arguments[i] = {this, i};
                pthread_t t;
                rc = pthread_create(&t, &attr, &Impl::entry, &arguments[i]);
                if (rc == EAGAIN || rc == ENOMEM)
                    throw ResourceLimit(std::string("pthread_create resource failure: ") +
                                        std::strerror(rc));
                if (rc)
                    throw std::runtime_error(std::string("pthread_create failed: ") +
                                             std::strerror(rc));
                threads.push_back(t);
            }
            pthread_attr_destroy(&attr);
            std::unique_lock<std::mutex> lock(control);
            done_cv.wait(lock, [&] { return started == workers; });
            if (startup_error)
                std::rethrow_exception(startup_error);
        } catch (...) {
            if (threads.size() != workers)
                pthread_attr_destroy(&attr);
            stop();
            throw;
        }
    }
    ~Impl() {
        stop();
    }
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(control);
            stopping = true;
        }
        start_cv.notify_all();
        wake_cv.notify_all();
        for (auto t : threads)
            pthread_join(t, nullptr);
        threads.clear();
    }
    static void *entry(void *ptr) noexcept {
        auto &a = *static_cast<ThreadArg *>(ptr);
        a.self->worker(a.id);
        return nullptr;
    }
    void notify_work() {
        wake_sequence.fetch_add(1, std::memory_order_release);
        wake_cv.notify_one();
    }
    void push(unsigned owner, std::size_t id) {
        auto &q = ready[owner];
        {
            std::lock_guard<std::mutex> lock(q.mutex);
            next[id] = none;
            prev[id] = q.tail;
            if (q.tail == none)
                q.head = id;
            else
                next[q.tail] = id;
            q.tail = id;
            ++q.count;
        }
        notify_work();
    }
    std::size_t pop(unsigned owner, bool steal) {
        auto &q = ready[owner];
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.head == none)
            return none;
        const auto id = steal ? q.head : q.tail;
        if (steal) {
            q.head = next[id];
            if (q.head == none)
                q.tail = none;
            else
                prev[q.head] = none;
        } else {
            q.tail = prev[id];
            if (q.tail == none)
                q.head = none;
            else
                next[q.tail] = none;
        }
        --q.count;
        return id;
    }
    void decrement(unsigned publisher, std::uint32_t target) {
        std::uint32_t old;
        if (options.backend == "atomic")
            old = remaining[target].fetch_sub(1, std::memory_order_acq_rel);
        else {
            old = remaining[target].load(std::memory_order_relaxed);
            if (old)
                remaining[target].store(old - 1, std::memory_order_relaxed);
        }
        if (!old)
            throw std::runtime_error("duplicate dependency notification or counter underflow");
        if (old == 1)
            push(publisher, target);
    }
    void send(unsigned owner, std::size_t edge) {
        auto &q = inbox[owner];
        {
            std::lock_guard<std::mutex> lock(q.mutex);
            event_next[edge] = none;
            if (q.tail == none)
                q.head = edge;
            else
                event_next[q.tail] = edge;
            q.tail = edge;
            ++q.count;
        }
        // Owner might be parked while another thread could consume generic ready work.
        wake_sequence.fetch_add(1, std::memory_order_release);
        wake_cv.notify_all();
    }
    bool poll(unsigned owner) {
        bool progress = false;
        for (unsigned i = 0; i < 256; ++i) {
            std::size_t edge;
            auto &q = inbox[owner];
            {
                std::lock_guard<std::mutex> lock(q.mutex);
                edge = q.head;
                if (edge == none)
                    break;
                q.head = event_next[edge];
                if (q.head == none)
                    q.tail = none;
                --q.count;
            }
            decrement(owner, graph->successors[edge]);
            progress = true;
        }
        return progress;
    }
    void fail(std::exception_ptr e) {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (!error)
                error = e;
        }
        failed.store(true, std::memory_order_release);
        wake_sequence.fetch_add(1, std::memory_order_release);
        wake_cv.notify_all();
    }
    void work(unsigned worker) {
        const auto n = graph->tasks.size();
        unsigned idle_rounds = 0, round_robin = worker;
        while (!failed.load(std::memory_order_acquire) &&
               completed.load(std::memory_order_acquire) < n) {
            const auto generation = wake_sequence.load(std::memory_order_acquire);
            bool progress = options.backend == "owner" ? poll(worker) : false;
            for (unsigned batch = 0; batch < options.batch; ++batch) {
                // Periodically take the oldest task even on the owner to prevent LIFO starvation.
                auto id = pop(worker, ((round_robin++) % 32) == 0);
                if (id == none) {
                    for (unsigned k = 1; k < workers; ++k) {
                        const auto other = (worker + k + (round_robin % workers)) % workers;
                        if (other == worker)
                            continue;
                        id = pop(other, true);
                        if (id != none) {
                            steals.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                    // Above rotated order can skip one victim; finish with a full sweep.
                    if (id == none)
                        for (unsigned other = 0; other < workers; ++other)
                            if (other != worker) {
                                id = pop(other, true);
                                if (id != none) {
                                    steals.fetch_add(1, std::memory_order_relaxed);
                                    break;
                                }
                            }
                }
                if (id == none)
                    break;
                if (failed.load(std::memory_order_acquire))
                    break;
                if (options.audit && seen[id].exchange(1, std::memory_order_acq_rel))
                    throw std::runtime_error("task executed twice");
                (*invoke)(graph->tasks[id]);
                for (auto e = graph->offsets[id]; e < graph->offsets[id + 1]; ++e) {
                    const auto target = graph->successors[e];
                    if (options.backend == "atomic")
                        decrement(worker, target);
                    else {
                        const auto owner = target % workers;
                        if (owner == worker)
                            decrement(worker, target);
                        else
                            send(owner, e);
                    }
                }
                const auto done = completed.fetch_add(1, std::memory_order_acq_rel) + 1;
                progress = true;
                if (done == n) {
                    wake_sequence.fetch_add(1, std::memory_order_release);
                    wake_cv.notify_all();
                }
                if (options.backend == "owner")
                    poll(worker);
                if (failed.load(std::memory_order_acquire))
                    break;
            }
            if (progress) {
                idle_rounds = 0;
                continue;
            }
            if (++idle_rounds < 32) {
                std::this_thread::yield();
                continue;
            }
            std::unique_lock<std::mutex> lock(wake_mutex);
            // Timed wait is a backup against a notification between the predicate check
            // and sleeping; generation check ensures rapid wakeups in the common case.
            wake_cv.wait_for(lock, std::chrono::microseconds(200), [&] {
                return failed.load(std::memory_order_acquire) ||
                       completed.load(std::memory_order_acquire) >= n ||
                       wake_sequence.load(std::memory_order_acquire) != generation;
            });
        }
    }
    void worker(unsigned id) noexcept {
        try {
            if (options.pin)
                pin_cpu(cpus[id]);
            else
                bind_node(cpus);
        } catch (...) {
            std::lock_guard<std::mutex> lock(control);
            if (!startup_error)
                startup_error = std::current_exception();
        }
        std::size_t observed = 0;
        std::unique_lock<std::mutex> lock(control);
        ++started;
        done_cv.notify_all();
        while (true) {
            start_cv.wait(lock, [&] { return stopping || epoch != observed; });
            if (stopping)
                return;
            observed = epoch;
            lock.unlock();
            try {
                work(id);
            } catch (...) {
                fail(std::current_exception());
            }
            lock.lock();
            if (--active == 0)
                done_cv.notify_all();
        }
    }
    std::size_t base_reservation() const {
        return checked_mul(workers, worker_stack_bytes + 2 * sizeof(Queue) + 4096);
    }
    std::size_t reservation_unlocked() const {
        auto bytes = base_reservation();
        bytes = checked_add(bytes,
                            checked_mul(next.capacity() + prev.capacity() + event_next.capacity(),
                                        sizeof(std::size_t)));
        bytes =
            checked_add(bytes, checked_mul(remaining_count, sizeof(std::atomic<std::uint32_t>)));
        bytes = checked_add(bytes, checked_mul(seen_count, sizeof(std::atomic<unsigned char>)));
        return bytes;
    }
    std::size_t reservation() const {
        std::lock_guard<std::mutex> lock(run_mutex);
        return reservation_unlocked();
    }
    void release_execution_storage() {
        std::vector<std::size_t>().swap(next);
        std::vector<std::size_t>().swap(prev);
        std::vector<std::size_t>().swap(event_next);
        remaining.reset();
        seen.reset();
        remaining_count = seen_count = 0;
    }
    void set_budget(std::size_t bytes) {
        std::lock_guard<std::mutex> lock(run_mutex);
        if (bytes < base_reservation())
            throw ResourceLimit("new memory budget cannot accommodate worker stacks");
        // Runs and budget changes serialize at the drained execution boundary.
        if (bytes < reservation_unlocked())
            release_execution_storage();
        options.memory_bytes = bytes;
    }
    ExecutionStats run(const Graph &g, const std::function<void(const Task &)> &call,
                       const std::function<void()> &before) {
        std::lock_guard<std::mutex> serialized(run_mutex);
        const auto begin = std::chrono::steady_clock::now();
        const auto n = g.tasks.size();
        if (n >= std::numeric_limits<std::uint32_t>::max())
            throw ResourceLimit("runtime task ID overflow");
        if (g.offsets.size() != n + 1 || g.indegrees.size() != n || g.offsets.empty() ||
            g.offsets.front() != 0 || g.offsets.back() != g.successors.size())
            throw std::runtime_error("invalid CSR graph");
        // Validate every range before traversing any adjacency, including future offsets.
        for (std::size_t i = 0; i < g.offsets.size(); ++i)
            if (g.offsets[i] > g.successors.size() || (i && g.offsets[i] < g.offsets[i - 1]))
                throw std::runtime_error("invalid CSR offset range");
        auto estimate = checked_add(
            g.memory_bytes, checked_mul(workers, worker_stack_bytes + 2 * sizeof(Queue) + 4096));
        estimate = checked_add(
            estimate, checked_mul(n, 2 * sizeof(std::size_t) + 2 * sizeof(std::uint32_t) +
                                         (options.audit ? sizeof(std::atomic<unsigned char>) : 0)));
        if (options.backend == "owner")
            estimate = checked_add(estimate, checked_mul(g.successors.size(), sizeof(std::size_t)));
        if (estimate > options.memory_bytes)
            throw ResourceLimit("graph and executor exceed runtime memory budget");
        // Previous execution has fully drained; release its arrays before new allocation.
        release_execution_storage();
        // This linear structural check is included in run time, and prevents hangs on malformed
        // input.
        std::vector<std::uint32_t> check(n, 0);
        for (std::size_t src = 0; src < n; ++src) {
            if (g.offsets[src] > g.offsets[src + 1])
                throw std::runtime_error("invalid CSR offsets");
            std::uint32_t last = 0;
            bool first = true;
            for (auto e = g.offsets[src]; e < g.offsets[src + 1]; ++e) {
                const auto dst = g.successors[e];
                if (dst <= src || dst >= n || (!first && dst <= last))
                    throw std::runtime_error("invalid, cyclic or duplicate graph edge");
                last = dst;
                first = false;
                ++check[dst];
            }
        }
        if (check != g.indegrees)
            throw std::runtime_error("incorrect graph indegrees");
        next.assign(n, none);
        prev.assign(n, none);
        remaining = std::make_unique<std::atomic<std::uint32_t>[]>(n);
        remaining_count = n;
        if (options.audit) {
            seen = std::make_unique<std::atomic<unsigned char>[]>(n);
            seen_count = n;
        }
        if (options.backend == "owner")
            event_next.assign(g.successors.size(), none);
        for (std::size_t i = 0; i < n; ++i) {
            remaining[i].store(g.indegrees[i], std::memory_order_relaxed);
            if (options.audit)
                seen[i].store(0, std::memory_order_relaxed);
        }
        for (unsigned i = 0; i < workers; ++i) {
            ready[i].head = ready[i].tail = inbox[i].head = inbox[i].tail = none;
            ready[i].count = inbox[i].count = 0;
        }
        // All graph validation and execution-storage allocation precedes the
        // first numerical operation, including an extracted input copy.
        if (before)
            before();
        graph = &g;
        invoke = &call;
        completed.store(0, std::memory_order_relaxed);
        steals.store(0, std::memory_order_relaxed);
        failed.store(false, std::memory_order_relaxed);
        error = nullptr;
        for (std::size_t i = 0; i < n; ++i)
            if (!g.indegrees[i])
                push(static_cast<unsigned>(i % workers), i);
        {
            std::unique_lock<std::mutex> lock(control);
            active = workers;
            ++epoch;
            start_cv.notify_all();
            done_cv.wait(lock, [&] { return active == 0; });
        }
        graph = nullptr;
        invoke = nullptr;
        if (error)
            std::rethrow_exception(error);
        if (completed.load(std::memory_order_acquire) != n)
            throw std::runtime_error("incomplete execution");
        if (options.audit)
            for (std::size_t i = 0; i < n; ++i)
                if (seen[i].load(std::memory_order_relaxed) != 1)
                    throw std::runtime_error("task not executed");
        ExecutionStats stats;
        stats.tasks = n;
        stats.edges = g.successors.size();
        stats.workers = workers;
        stats.steals = steals.load(std::memory_order_relaxed);
        stats.execute_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        return stats;
    }
};
Executor::Executor(RuntimeOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Executor::~Executor() = default;
ExecutionStats Executor::run(const Graph &g, const std::function<void(const Task &)> &fn,
                             const std::function<void()> &before) {
    return impl_->run(g, fn, before);
}
void Executor::set_memory_budget(std::size_t bytes) {
    impl_->set_budget(bytes);
}
std::size_t Executor::reserved_bytes() const {
    return impl_->reservation();
}
} // namespace dag
