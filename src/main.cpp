#include "dag/core.hpp"
#include "dag/resources.hpp"
#include "kernels.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#ifndef DAG_KERNEL_MODE
#define DAG_KERNEL_MODE "unspecified-local-build"
#endif

namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}
std::string env(const char *name, const std::string &fallback = "") {
    const char *value = std::getenv(name);
    return value ? value : fallback;
}
std::size_t number(const char *name, std::size_t fallback, std::size_t max) {
    auto s = env(name);
    if (s.empty())
        return fallback;
    if (s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(std::string(name) + " must be a nonnegative integer");
    std::size_t n = 0;
    for (char c : s) {
        if (static_cast<unsigned>(c - '0') > max || n > (max - static_cast<unsigned>(c - '0')) / 10)
            throw std::runtime_error(std::string(name) + " out of range");
        n = n * 10 + static_cast<unsigned>(c - '0');
    }
    if (n > max)
        throw std::runtime_error(std::string(name) + " out of range");
    return n;
}
std::string quoted(const std::string &text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << static_cast<unsigned>(c) << std::dec;
        else
            out << c;
    }
    out << '"';
    return out.str();
}
std::uint32_t block_value(const std::string &text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("block sizes must be positive decimal integers");
    const auto value = std::stoull(text);
    if (!value || value > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
        throw std::runtime_error("block size out of ABI range");
    return static_cast<std::uint32_t>(value);
}
void separate_paths(const std::vector<std::string> &paths) {
    for (std::size_t i = 0; i < paths.size(); ++i)
        if (!paths[i].empty())
            for (std::size_t j = 0; j < i; ++j)
                if (!paths[j].empty()) {
                    std::error_code ec;
                    const bool same = std::filesystem::equivalent(paths[i], paths[j], ec);
                    if ((!ec && same) || std::filesystem::weakly_canonical(paths[i]) ==
                                             std::filesystem::weakly_canonical(paths[j]))
                        throw std::runtime_error(
                            "input, output, metadata and report paths must be distinct");
                }
}
void pin_main(const std::vector<unsigned> &cpus) {
    if (cpus.empty())
        throw std::runtime_error("empty allowed CPU set");
    const auto count = static_cast<std::size_t>(*std::max_element(cpus.begin(), cpus.end())) + 1;
    const auto bytes = CPU_ALLOC_SIZE(count);
    auto *mask = CPU_ALLOC(count);
    if (!mask)
        throw std::bad_alloc();
    CPU_ZERO_S(bytes, mask);
    for (auto cpu : cpus)
        CPU_SET_S(cpu, bytes, mask);
    const int status = sched_setaffinity(0, bytes, mask);
    CPU_FREE(mask);
    if (status)
        throw std::runtime_error("cannot bind main thread to permitted NUMA CPUs");
}
struct IOTime {
    double &seconds;
    Clock::time_point begin = Clock::now();
    ~IOTime() {
        seconds += elapsed(begin);
    }
};
void read(std::ifstream &f, void *p, std::size_t bytes, double &io) {
    IOTime t{io};
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("read size overflow");
    f.read(static_cast<char *>(p), static_cast<std::streamsize>(bytes));
    if (!f)
        throw std::runtime_error("truncated input");
}
void write(std::ofstream &f, const void *p, std::size_t bytes, double &io) {
    IOTime t{io};
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("write size overflow");
    f.write(static_cast<const char *>(p), static_cast<std::streamsize>(bytes));
    if (!f)
        throw std::runtime_error("output write failed");
}
struct TempOutput {
    std::string path;
    bool committed = false;
    explicit TempOutput(const std::string &dest) {
        std::string pattern = dest + ".tmp.XXXXXX";
        std::vector<char> p(pattern.begin(), pattern.end());
        p.push_back(0);
        int fd = ::mkstemp(p.data());
        if (fd < 0)
            throw std::runtime_error("cannot create temporary output");
        ::close(fd);
        path = p.data();
    }
    ~TempOutput() {
        if (!committed) {
            std::error_code e;
            std::filesystem::remove(path, e);
        }
    }
};
bool region_equals(const dag::Region &r, const char *buffer, std::size_t offset, std::size_t rows,
                   std::size_t cols, std::size_t stride) {
    return r.buffer == buffer && r.offset == offset && r.rows == rows && r.cols == cols &&
           r.stride == stride;
}
void require_region(const std::vector<dag::Region> &rs, const char *buffer, std::size_t offset,
                    std::size_t rows, std::size_t cols, std::size_t stride) {
    if (!std::any_of(rs.begin(), rs.end(), [&](const dag::Region &r) {
            return region_equals(r, buffer, offset, rows, cols, stride);
        }))
        throw std::runtime_error("metadata access contract does not cover bound official operator");
}
// General postfix expression adapter for the explicitly selected scalar
// application. It is never admitted by the default official-blocked binding.
void scalar_expression(const dag::Task &t, std::size_t n, const double *a = nullptr,
                       double *l = nullptr) {
    const bool execute = l != nullptr;
    if (t.args.size() < 2)
        throw std::runtime_error("empty scalar expression");
    const auto index = [&](dag::Int value) {
        if (value < 0 || static_cast<std::uint64_t>(value) >= n * n)
            throw std::runtime_error("scalar element offset outside matrix");
        return static_cast<std::size_t>(value);
    };
    const auto dest = index(t.args[0]);
    if (!execute)
        require_region(t.writes, "L", dest, 1, 1, n);
    thread_local std::vector<double> stack;
    if (execute) {
        stack.clear();
        stack.reserve(t.args.size());
    }
    std::size_t depth = 0;
    const auto push = [&](double value) {
        if (!std::isfinite(value))
            throw std::runtime_error("non-finite scalar expression result");
        stack.push_back(value);
    };
    for (std::size_t pc = 1; pc < t.args.size();) {
        const auto op = t.args[pc++];
        if (op == 0 || op == 1) {
            if (pc == t.args.size())
                throw std::runtime_error("truncated scalar load");
            const auto offset = index(t.args[pc++]);
            if (execute)
                push(op == 0 ? a[offset] : l[offset]);
            else
                require_region(t.reads, op == 0 ? "A" : "L", offset, 1, 1, n);
            ++depth;
        } else if (op == 2) {
            if (t.args.size() - pc < 4)
                throw std::runtime_error("truncated scalar literal");
            std::uint64_t bits = 0;
            for (unsigned word = 0; word < 4; ++word) {
                const auto value = t.args[pc++];
                if (value < 0 || value > 65535)
                    throw std::runtime_error("invalid scalar literal word");
                bits |= static_cast<std::uint64_t>(value) << (word * 16);
            }
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value))
                throw std::runtime_error("non-finite scalar literal");
            if (execute)
                push(value);
            ++depth;
        } else if (op >= 3 && op <= 8) {
            const bool unary = op >= 7;
            if (depth < (unary ? 1u : 2u))
                throw std::runtime_error("scalar expression stack underflow");
            if (!unary)
                --depth;
            if (execute) {
                const auto right = stack.back();
                stack.pop_back();
                if (unary)
                    push(op == 7 ? std::sqrt(right) : -right);
                else {
                    const auto left = stack.back();
                    stack.pop_back();
                    if (op == 3) push(left + right);
                    else if (op == 4) push(left - right);
                    else if (op == 5) push(left * right);
                    else push(left / right);
                }
            }
        } else
            throw std::runtime_error("unknown scalar opcode");
    }
    if (depth != 1)
        throw std::runtime_error("scalar expression must produce one value");
    if (execute)
        l[dest] = stack.back();
}
void validate_task(const dag::Task &t, std::uint32_t n, std::uint32_t b) {
    const auto nn = static_cast<std::size_t>(n) * n;
    for (const auto *regions : {&t.reads, &t.writes})
        for (const auto &r : *regions) {
            if ((r.buffer != "A" && r.buffer != "L") || !r.rows || !r.cols || r.stride < n ||
                r.cols > r.stride || r.offset >= nn ||
                r.rows - 1 > (nn - 1 - r.offset) / r.stride ||
                r.cols > nn - r.offset - (r.rows - 1) * r.stride)
                throw std::runtime_error("metadata region out of matrix bounds");
        }
    if (t.symbol == "scalar_expression") {
        scalar_expression(t, n);
        return;
    }
    if (t.symbol == "__copy") {
        if (!t.args.empty())
            throw std::runtime_error("copy takes no arguments");
        require_region(t.reads, "A", 0, n, n, n);
        require_region(t.writes, "L", 0, n, n, n);
        return;
    }
    if (t.symbol == "__zero_upper") {
        if (!t.args.empty())
            throw std::runtime_error("zero_upper takes no arguments");
        // The decoder emits one region per row in order. Verify that common
        // representation once, without searching the whole list for every row.
        // Keep the general coverage check for reordered or redundant regions.
        bool canonical = t.writes.size() == static_cast<std::size_t>(n - 1);
        for (std::size_t row = 0; canonical && row + 1 < n; ++row)
            canonical = region_equals(t.writes[row], "L", row * n + row + 1,
                                      1, n - row - 1, n);
        if (!canonical)
            for (std::size_t row = 0; row + 1 < n; ++row)
                require_region(t.writes, "L", row * n + row + 1, 1, n - row - 1, n);
        return;
    }
    std::size_t argc =
        t.symbol == "cholesky" ? 4 : (t.symbol == "trsm" || t.symbol == "madd" ? 5 : 0);
    if (!argc || t.args.size() != argc)
        throw std::runtime_error("unknown operator or invalid argument count: " + t.symbol);
    if (t.args[argc - 2] != b || t.args[argc - 1] != n)
        throw std::runtime_error("operator block size/stride does not match case");
    for (std::size_t i = 0; i < argc - 2; ++i) {
        if (t.args[i] < 0)
            throw std::runtime_error("negative matrix offset");
        auto offset = static_cast<std::size_t>(t.args[i]);
        if (offset >= nn || offset / n + b > n || offset % n + b > n)
            throw std::runtime_error("operator block outside matrix");
    }
    const auto overlaps = [&](std::size_t x, std::size_t y) {
        const auto xr = x / n, xc = x % n, yr = y / n, yc = y % n;
        return xr < yr + b && yr < xr + b && xc < yc + b && yc < xc + b;
    };
    const auto a = static_cast<std::size_t>(t.args[0]);
    const auto c = static_cast<std::size_t>(t.args[argc - 3]);
    if (t.symbol == "cholesky" && a != c && overlaps(a, c))
        throw std::runtime_error("unsupported partial cholesky alias");
    if (t.symbol == "trsm") {
        const auto factor = static_cast<std::size_t>(t.args[1]);
        if ((a != c && overlaps(a, c)) || overlaps(factor, c) || overlaps(factor, a))
            throw std::runtime_error("unsupported trsm alias");
    }
    if (t.symbol == "madd") {
        const auto input_b = static_cast<std::size_t>(t.args[1]);
        if ((a != input_b && overlaps(a, input_b)) || overlaps(c, a) || overlaps(c, input_b))
            throw std::runtime_error("unsupported madd alias");
    }
    require_region(t.reads, "L", a, b, b, n);
    if (argc == 5)
        require_region(t.reads, "L", static_cast<std::size_t>(t.args[1]), b, b, n);
    if (t.symbol == "madd")
        require_region(t.reads, "L", c, b, b, n);
    require_region(t.writes, "L", c, b, b, n);
}
void invoke(const dag::Task &t, const std::vector<double> &a, std::vector<double> &l, int n,
            int b) {
    if (t.symbol == "scalar_expression") {
        scalar_expression(t, static_cast<std::size_t>(n), a.data(), l.data());
        return;
    }
    if (t.symbol == "__copy") {
        std::copy(a.begin(), a.end(), l.begin());
        return;
    }
    if (t.symbol == "__zero_upper") {
        for (int row = 0; row < n; ++row)
            std::fill(l.begin() + static_cast<std::size_t>(row) * n + row + 1,
                      l.begin() + static_cast<std::size_t>(row + 1) * n, 0.0);
        return;
    }
    double *p = l.data();
    if (t.symbol == "cholesky")
        cholesky(p + t.args[0], p + t.args[1], b, n);
    else if (t.symbol == "trsm")
        trsm(p + t.args[0], p + t.args[1], p + t.args[2], b, n);
    else if (t.symbol == "madd")
        madd(p + t.args[0], p + t.args[1], p + t.args[2], b, n);
    else
        throw std::runtime_error("unbound operator: " + t.symbol);
}
struct CaseStats {
    std::uint32_t n, b;
    std::uint64_t tasks, edges, steals, phase_tasks;
    unsigned workers;
    double compute, prepare, execute;
    std::size_t graph_bytes;
    std::string execution_mode;
    bool resource_limited = false;
    std::string resource_reason, resource_stage;
    std::size_t effective_graph_budget_bytes = 0;
    std::size_t case_memory_budget_bytes = 0;
};
void write_text(const std::string &path, const std::string &value) {
    if (path.empty())
        return;
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("cannot open report: " + path);
    f << value;
    f.close();
    if (!f)
        throw std::runtime_error("cannot write report: " + path);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.bin output.bin\n";
        return 1;
    }
    const auto total_start = Clock::now();
    double io_seconds = 0;
    std::optional<std::uint32_t> active_case;
    std::uint32_t active_n = 0, active_b = 0;
    std::string resource_stage = "configuration";
    const auto report_resource = [&](const char *reason) {
        std::cerr << "DAG_RESOURCE_LIMIT {\"index\":";
        if (active_case) std::cerr << *active_case;
        else std::cerr << "null";
        std::cerr << ",\"n\":" << active_n << ",\"b\":" << active_b
                  << ",\"stage\":" << quoted(resource_stage)
                  << ",\"reason\":" << quoted(reason) << "}\n";
    };
    try {
        static_assert(sizeof(double) == 8 && sizeof(std::uint32_t) == 4,
                      "unsupported matrix representation");
        const auto metadata_path = env("DAG_METADATA", "block_cholesky.meta");
        const auto timing_path = env("COMPILER2026_TIMING_FILE");
        const auto stats_path = env("DAG_STATS_FILE");
        separate_paths({argv[1], argv[2], metadata_path, timing_path, stats_path});
        const auto io_format = env("DAG_IO_FORMAT", "public");
        if (io_format != "public" && io_format != "original")
            throw std::runtime_error("DAG_IO_FORMAT must be public or original");
        const bool original_format = io_format == "original";
        const auto application = env("DAG_APPLICATION", "blocked");
        if (application != "blocked" && application != "scalar")
            throw std::runtime_error("DAG_APPLICATION must be blocked or scalar");
        const bool scalar_application = application == "scalar";
        const auto block_text = env("DAG_BLOCK_SIZE"), blocks_text = env("DAG_BLOCK_SIZES");
        if (!block_text.empty() && !blocks_text.empty())
            throw std::runtime_error("choose DAG_BLOCK_SIZE or DAG_BLOCK_SIZES, not both");
        if ((!original_format || scalar_application) &&
            (!block_text.empty() || !blocks_text.empty()))
            throw std::runtime_error("block overrides only apply to original-format blocked execution");
        if (original_format && !scalar_application && block_text.empty() && blocks_text.empty())
            throw std::runtime_error("original blocked input requires explicit DAG_BLOCK_SIZE or DAG_BLOCK_SIZES");
        auto mode = env("DAG_MODE", "auto");
        if (mode != "serial" && mode != "parallel" && mode != "auto")
            throw std::runtime_error("DAG_MODE must be serial, parallel or auto");
        dag::RuntimeOptions options;
        options.threads = static_cast<unsigned>(number("DAG_THREADS", 0, 4096));
        options.batch = static_cast<unsigned>(number("DAG_BATCH", 1, 1024));
        if (!options.batch)
            throw std::runtime_error("DAG_BATCH must be positive");
        options.backend = env("DAG_BACKEND", "atomic");
        if (options.backend != "atomic" && options.backend != "owner")
            throw std::runtime_error("DAG_BACKEND must be atomic or owner");
        options.audit = number("DAG_AUDIT", 0, 1) != 0;
        const auto memory_mb =
            number("DAG_MEMORY_MB", 0, std::numeric_limits<std::size_t>::max() / (1024 * 1024));
        if (!memory_mb && !env("DAG_MEMORY_MB").empty())
            throw std::runtime_error("DAG_MEMORY_MB must be positive when set");
        const auto requested_memory = memory_mb * 1024 * 1024;
        const auto permitted_cpus = dag::allowed_node_cpus();
        const auto memory = dag::available_memory_budget(requested_memory, permitted_cpus);
        if (options.threads > permitted_cpus.size())
            throw std::runtime_error("DAG_THREADS exceeds allowed single-NUMA CPU set");
        pin_main(permitted_cpus);
        options.memory_bytes = memory;
        dag::Limits limits;
        limits.memory_bytes = memory;
        limits.max_tasks = number("DAG_MAX_TASKS", 0, std::numeric_limits<std::size_t>::max());
        limits.max_steps = number("DAG_MAX_STEPS", 0, std::numeric_limits<std::size_t>::max());
        const auto graph_mb = number("DAG_GRAPH_MEMORY_MB", 0,
                                    std::numeric_limits<std::size_t>::max() / (1024 * 1024));
        const auto graph_budget = graph_mb * 1024 * 1024;
        resource_stage = "metadata";
        dag::Program program = dag::load_metadata(metadata_path, limits);
        const auto metadata_bytes = program.artifact_bytes;
        const auto source_digest = program.source_digest, contract_digest = program.contract_digest;
        for (const auto &op : program.operators)
            if (op != "__copy" && op != "__zero_upper" &&
                (scalar_application ? op != "scalar_expression"
                                    : (op != "cholesky" && op != "trsm" && op != "madd")))
                throw std::runtime_error("operator is not bound by this application: " + op);
        if (program.retained_bytes > memory || memory - program.retained_bytes < 1024 * 1024)
            throw dag::ResourceLimit("metadata exceeds application memory budget");
        const auto metadata_reserve =
            program.retained_bytes + 1024 * 1024; // reports, stream and allocator slack
        std::ifstream input;
        std::unique_ptr<TempOutput> temp;
        std::ofstream output;
        {
            IOTime time{io_seconds};
            input.open(argv[1], std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot open input");
            temp = std::make_unique<TempOutput>(argv[2]);
            output.open(temp->path, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("cannot open output");
        }
        std::uint32_t count = 0;
        read(input, &count, sizeof(count), io_seconds);
        std::uintmax_t file_bytes = 0;
        {
            IOTime time{io_seconds};
            file_bytes = std::filesystem::file_size(argv[1]);
        }
        const std::uintmax_t case_header_bytes = original_format ? 4 : 8;
        if (file_bytes < 4 || count > (file_bytes - 4) / (case_header_bytes + sizeof(double)))
            throw std::runtime_error("impossible input case count");
        std::vector<std::uint32_t> block_sizes;
        const auto shared_block = block_text.empty() ? 0u : block_value(block_text);
        if (!blocks_text.empty()) {
            if (blocks_text.back() == ',')
                throw std::runtime_error("empty per-case block size");
            std::istringstream fields(blocks_text);
            std::string field;
            while (std::getline(fields, field, ',')) {
                if (block_sizes.size() >= count ||
                    block_sizes.size() >= memory / sizeof(std::uint32_t) / 8)
                    throw std::runtime_error("too many per-case block sizes");
                block_sizes.push_back(block_value(field));
            }
            if (block_sizes.size() != count)
                throw std::runtime_error("DAG_BLOCK_SIZES must have one entry per case");
        }
        write(output, &count, sizeof(count), io_seconds);
        std::uintmax_t consumed_bytes = 4;
        std::unique_ptr<dag::Executor> executor;
        std::size_t retained_runtime_bytes = 0;
        std::vector<CaseStats> stats;
        if (count > memory / sizeof(CaseStats) / 8)
            throw std::runtime_error("case statistics exceed budget");
        stats.reserve(count);
        std::vector<double> a, l;
        for (std::uint32_t index = 0; index < count; ++index) {
            active_case = index;
            resource_stage = "input-header";
            std::uint32_t n = 0, b = 0;
            read(input, &n, 4, io_seconds);
            if (original_format)
                b = scalar_application ? 1u : (block_sizes.empty() ? shared_block : block_sizes[index]);
            else
                read(input, &b, 4, io_seconds);
            active_n = n; active_b = b;
            resource_stage = "matrix-budget";
            consumed_bytes += case_header_bytes;
            auto case_start = Clock::now();
            double io_before = io_seconds;
            if (!n || !b || b > n || n % b)
                throw std::runtime_error("n/b must be positive and divisible");
            // The supplied official implementation computes row*lda+col using int.
            if (static_cast<std::uint64_t>(n) * n >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("matrix dimension exceeds official int indexing range");
            const auto elements = static_cast<std::size_t>(n) * n;
            // Reuse adjacent equal shapes, without retaining a larger previous
            // allocation when the next case shrinks or grows.
            if (a.size() != elements) {
                std::vector<double>().swap(a);
                std::vector<double>().swap(l);
            }
            if (consumed_bytes > file_bytes ||
                elements > (file_bytes - consumed_bytes) / sizeof(double))
                throw std::runtime_error("truncated matrix payload");
            consumed_bytes += elements * sizeof(double);
            if (elements > (memory - metadata_reserve) / sizeof(double) / 2)
                throw dag::ResourceLimit("matrix buffers exceed memory budget");
            const auto matrix_bytes = std::max(elements, a.capacity()) * sizeof(double) +
                                      std::max(elements, l.capacity()) * sizeof(double);
            if (matrix_bytes > memory - metadata_reserve)
                throw dag::ResourceLimit("retained matrix capacity exceeds memory budget");
            const auto auxiliary = stats.capacity() * sizeof(CaseStats) +
                                   block_sizes.capacity() * sizeof(std::uint32_t);
            if (auxiliary > memory - metadata_reserve - matrix_bytes)
                throw dag::ResourceLimit("application memory budget exhausted");
            // Refresh headroom for this case. The probe reports future capacity,
            // so add back our already retained allocations before comparing it
            // with the process-wide ceiling; do not subtract them twice.
            const auto retained_matrices = (a.capacity() + l.capacity()) * sizeof(double);
            const auto managed = metadata_reserve + auxiliary + retained_runtime_bytes + retained_matrices;
            const auto fresh = dag::available_memory_budget(0, permitted_cpus);
            const auto fresh_total = fresh > std::numeric_limits<std::size_t>::max() - managed
                ? std::numeric_limits<std::size_t>::max() : fresh + managed;
            const auto case_total = std::min(memory, fresh_total);
            if (metadata_reserve > case_total || matrix_bytes > case_total - metadata_reserve ||
                auxiliary > case_total - metadata_reserve - matrix_bytes)
                throw dag::ResourceLimit("current available memory cannot fit matrix buffers");
            const auto case_budget = case_total - metadata_reserve - matrix_bytes - auxiliary;
            if (case_budget < 4096)
                throw dag::ResourceLimit("matrix buffers leave insufficient execution memory");
            limits.memory_bytes = case_budget;
            const auto workspace = [&](const dag::Task &task) -> std::size_t {
                // ABI workspace estimates belong to the operator binding, not
                // to an algorithm formula. The demo's only block scratch is trsm.
                if (task.symbol == "trsm")
                    return static_cast<std::size_t>(b) * b * sizeof(double);
                if (task.symbol == "scalar_expression")
                    return task.args.size() * sizeof(double);
                return 0;
            };
            const auto planned_workers = options.threads ? options.threads
                : static_cast<unsigned>(permitted_cpus.size());
            bool stream = mode == "serial" || (mode == "auto" && planned_workers == 1);
            if (executor && (stream || retained_runtime_bytes > case_budget / 2)) {
                executor.reset();
                retained_runtime_bytes = 0;
            }
            resource_stage = "matrix-allocation";
            if (a.empty()) {
                a.resize(elements);
                l.resize(elements);
            } else if (program.nodes.empty() || program.nodes.front().kind != dag::Node::Kind::Copy) {
                // Preserve the original zero-initialized output semantics unless
                // the first semantic action provably overwrites the full buffer.
                std::fill(l.begin(), l.end(), 0.0);
            }
            read(input, a.data(), elements * sizeof(double), io_seconds);
            if (!std::all_of(a.begin(), a.end(), [](double x) { return std::isfinite(x); }))
                throw std::runtime_error("non-finite input matrix");
            CaseStats st{n, b, 0, 0, 0, 0, 1, 0, 0, 0, 0, "serial-stream", false, {}, {}, 0, 0};
            st.case_memory_budget_bytes = case_budget;
            const auto kernel = [&](const dag::Task &task) {
                if (workspace(task) > case_budget - 4096)
                    throw dag::ResourceLimit("kernel workspace exceeds available execution memory");
                invoke(task, a, l, static_cast<int>(n), static_cast<int>(b));
            };
            if (!stream) {
                const auto prep_start = Clock::now();
                bool numerical_started = false;
                const auto fallback = [&](const char *reason) {
                    // The failed candidate has unwound before memory is reused.
                    // Never replay after copy, a public call, or any scalar store.
                    if (numerical_started)
                        throw;
                    executor.reset();
                    retained_runtime_bytes = 0;
                    st.tasks = st.edges = st.steals = st.phase_tasks = 0;
                    st.graph_bytes = 0;
                    st.workers = 1;
                    st.execution_mode = "serial-resource";
                    st.resource_limited = true;
                    st.resource_reason = reason;
                    st.resource_stage = resource_stage;
                    st.prepare = elapsed(prep_start);
                    stream = true;
                };
                try {
                    if (executor)
                        executor->set_memory_budget(case_budget);
                    retained_runtime_bytes = executor ? executor->reserved_bytes() : 0;
                    if (retained_runtime_bytes >= case_budget)
                        throw dag::ResourceLimit("retained executor exceeds preparation budget");
                    dag::Limits preparation_limits = limits;
                    // Reserve worker stacks before expansion, even on the first case.
                    // The runtime supplies its own reservation formula; do not bake
                    // this host's worker count or a fixed graph capacity into it.
                    const auto desired_pool = std::max(retained_runtime_bytes,
                        dag::Executor::minimum_reserved_bytes(planned_workers));
                    // A tiny instance may select serial execution and need no
                    // pool at all. Defer a pool-capacity failure until that
                    // decision is known; account existing allocations regardless.
                    const auto pool_reserve = desired_pool < case_budget
                        ? desired_pool : retained_runtime_bytes;
                    const auto cap_graph = [&](std::size_t available) {
                        return graph_budget ? std::min(graph_budget, available) : available;
                    };
                    // Expansion does not invoke kernels. Their scratch is only
                    // reserved after the actual bound calls are known; a symbol
                    // in the catalog need not execute for this shape.
                    preparation_limits.memory_bytes = cap_graph(case_budget - pool_reserve);
                    st.effective_graph_budget_bytes = preparation_limits.memory_bytes;
                    resource_stage = "task-expansion";
                    // This bounded speculative expansion stops as soon as its
                    // representation no longer fits. It never runs numerical work.
                    auto tasks = dag::instantiate(program, {n, b}, preparation_limits);
                    resource_stage = "task-validation";
                    std::size_t scratch_per_worker = 0, numerical_calls = 0;
                    long double work_units = 0;
                    for (const auto &task : tasks) {
                        validate_task(task, n, b);
                        scratch_per_worker = std::max(scratch_per_worker, workspace(task));
                        if (task.symbol != "__copy" && task.symbol != "__zero_upper") {
                            ++numerical_calls;
                            const long double cube = static_cast<long double>(b) * b * b;
                            if (task.symbol == "cholesky") work_units += cube / 3;
                            else if (task.symbol == "trsm") work_units += cube;
                            else if (task.symbol == "madd") work_units += 2 * cube;
                            else work_units += task.args.size();
                        }
                    }
                    // Few calls can still be expensive: more available cores
                    // must not turn a heavy DAG into a serial computation.
                    if (mode == "auto" && (numerical_calls <= 1 ||
                        (numerical_calls <= 2 * static_cast<std::size_t>(planned_workers) &&
                         work_units <= 1048576.0L))) {
                        resource_stage = "serial-small-execution";
                        st.execution_mode = "serial-small-workload";
                        st.prepare = elapsed(prep_start);
                        const auto exec_start = Clock::now();
                        numerical_started = true;
                        for (const auto &task : tasks) {
                            kernel(task);
                            ++st.tasks;
                        }
                        st.execute = elapsed(exec_start);
                    } else {
                        resource_stage = "parallel-workspace";
                        st.execution_mode = "parallel";
                        if (scratch_per_worker > case_budget / planned_workers)
                            throw dag::ResourceLimit("parallel kernel workspace exceeds memory budget");
                        const auto parallel_budget = case_budget - scratch_per_worker * planned_workers;
                        if (executor)
                            executor->set_memory_budget(parallel_budget);
                        std::optional<dag::Task> pre_stage, post_stage;
                        if (!tasks.empty() && tasks.front().symbol == "__copy") {
                            pre_stage = std::move(tasks.front());
                            tasks.erase(tasks.begin());
                            ++st.phase_tasks;
                        }
                        if (!tasks.empty() && tasks.back().symbol == "__zero_upper") {
                            post_stage = std::move(tasks.back());
                            tasks.pop_back();
                            ++st.phase_tasks;
                        }
                        resource_stage = "worker-pool";
                        if (!executor) {
                            if (dag::Executor::minimum_reserved_bytes(planned_workers) >= parallel_budget)
                                throw dag::ResourceLimit("worker reservation exceeds parallel budget");
                            options.memory_bytes = parallel_budget;
                            executor = std::make_unique<dag::Executor>(options);
                        }
                        retained_runtime_bytes = executor->reserved_bytes();
                        if (retained_runtime_bytes >= parallel_budget)
                            throw dag::ResourceLimit("executor exceeds graph preparation budget");
                        preparation_limits.memory_bytes =
                            cap_graph(parallel_budget - retained_runtime_bytes);
                        st.effective_graph_budget_bytes = preparation_limits.memory_bytes;
                        resource_stage = "graph-construction";
                        auto graph = dag::build_graph(std::move(tasks), preparation_limits);
                        if (options.audit)
                            dag::validate_graph(graph);
                        executor->set_memory_budget(parallel_budget);
                        st.prepare = elapsed(prep_start);
                        st.graph_bytes = graph.memory_bytes;
                        resource_stage = "execution-storage";
                        auto result = executor->run(graph, kernel, [&] {
                            resource_stage = "parallel-execution";
                            numerical_started = true;
                            if (pre_stage)
                                kernel(*pre_stage);
                        });
                        st.tasks = result.tasks + st.phase_tasks;
                        st.edges = result.edges;
                        st.steals = result.steals;
                        st.workers = result.workers;
                        st.execute = result.execute_seconds;
                        if (post_stage) {
                            const auto post_start = Clock::now();
                            kernel(*post_stage);
                            st.execute += elapsed(post_start);
                        }
                        retained_runtime_bytes = executor->reserved_bytes();
                    }
                } catch (const dag::ResourceLimit &error) {
                    fallback(error.what());
                } catch (const std::bad_alloc &error) {
                    fallback(error.what());
                }
            }
            if (stream) {
                resource_stage = "serial-execution";
                const auto exec_start = Clock::now();
                dag::enumerate_program(program, {n, b}, limits, [&](const dag::Task &task) {
                    validate_task(task, n, b);
                    kernel(task);
                    ++st.tasks;
                });
                st.execute = elapsed(exec_start);
            }
            // Result construction is timed; actual matrix file writes alone are excluded.
            write(output, &n, 4, io_seconds);
            if (!original_format)
                write(output, &b, 4, io_seconds);
            write(output, l.data(), elements * sizeof(double), io_seconds);

            st.compute = elapsed(case_start) - (io_seconds - io_before);
            stats.push_back(st);
        }
        {
            IOTime time{io_seconds};
            char extra;
            if (input.get(extra))
                throw std::runtime_error("trailing input data");
            if (!input.eof())
                throw std::runtime_error("input read error");
            input.close();
            output.flush();
            if (!output)
                throw std::runtime_error("output flush failed");
            output.close();
            if (!output)
                throw std::runtime_error("output close failed");
        }
        resource_stage = "finalize";
        active_case.reset();
        std::vector<double>().swap(a);
        std::vector<double>().swap(l);
        executor.reset();
        {
            IOTime time{io_seconds};
            std::filesystem::rename(temp->path, argv[2]);
            temp->committed = true;
        }
        // Destroy metadata and common algorithm resources before stopping the score timer.
        program = dag::Program{};
        const double score = elapsed(total_start) - io_seconds;
        double case_sum = 0;
        for (const auto &st : stats)
            case_sum += st.compute;
        std::ostringstream timing;
        timing << std::setprecision(17) << score << '\n';
        write_text(timing_path, timing.str());
        std::ostringstream report;
        report << std::setprecision(17) << "{\"kernel_mode\":" << quoted(DAG_KERNEL_MODE)
               << ",\"application\":" << quoted(application)
               << ",\"io_format\":" << quoted(io_format)
               << ",\"mode\":" << quoted(mode) << ",\"backend\":" << quoted(options.backend)
               << ",\"memory_budget_bytes\":" << memory
               << ",\"requested_memory_bytes\":" << requested_memory
               << ",\"graph_budget_bytes\":" << graph_budget
               << ",\"graph_budget_source\":" << quoted(graph_budget ? "explicit-cap" : "automatic")
               << ",\"memory_budget_source\":" << quoted(requested_memory ? "explicit-cap" : "automatic")
               << ",\"threads_requested\":" << options.threads
               << ",\"batch\":" << options.batch
               << ",\"max_tasks\":" << limits.max_tasks
               << ",\"max_steps\":" << limits.max_steps
               << ",\"metadata_bytes\":" << metadata_bytes
               << ",\"source_digest\":" << quoted(source_digest)
               << ",\"contract_digest\":" << quoted(contract_digest)
               << ",\"score_seconds\":" << score << ",\"matrix_io_seconds\":" << io_seconds
               << ",\"common_finalize_seconds\":" << std::max(0.0, score - case_sum)
               << ",\"cpus\":[";
        for (std::size_t i = 0; i < permitted_cpus.size(); ++i) {
            if (i)
                report << ',';
            report << permitted_cpus[i];
        }
        report << "],\"cases\":[";
        for (std::size_t i = 0; i < stats.size(); ++i) {
            const auto &s = stats[i];
            if (i)
                report << ',';
            report << "{\"index\":" << i << ",\"n\":" << s.n << ",\"b\":" << s.b
                   << ",\"execution_mode\":" << quoted(s.execution_mode)
                   << ",\"resource_limited\":" << (s.resource_limited ? "true" : "false")
                   << ",\"resource_reason\":" << quoted(s.resource_reason)
                   << ",\"resource_stage\":" << quoted(s.resource_stage)
                   << ",\"effective_graph_budget_bytes\":" << s.effective_graph_budget_bytes
                   << ",\"case_memory_budget_bytes\":" << s.case_memory_budget_bytes
                   << ",\"tasks\":" << s.tasks
                   << ",\"phase_tasks\":" << s.phase_tasks << ",\"edges\":" << s.edges
                   << ",\"workers\":" << s.workers << ",\"steals\":" << s.steals
                   << ",\"graph_bytes\":" << s.graph_bytes << ",\"prepare_seconds\":" << s.prepare
                   << ",\"execute_seconds\":" << s.execute << ",\"compute_seconds\":" << s.compute
                   << '}';
        }
        report << "]}\n";
        write_text(stats_path, report.str());
        std::cerr << "cases=" << count << " score_seconds=" << std::setprecision(9) << score
                  << " matrix_io_seconds=" << io_seconds << '\n';
        return 0;
    } catch (const dag::ResourceLimit &e) {
        report_resource(e.what());
        return 2;
    } catch (const std::bad_alloc &e) {
        report_resource(e.what());
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "dag_run failed: " << e.what() << '\n';
        return 1;
    }
}
