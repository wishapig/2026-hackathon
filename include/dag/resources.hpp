#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace dag {
namespace resource_detail {
inline std::optional<std::size_t> kib_field(const std::filesystem::path &path,
                                           const std::string &key) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto pos = line.find(key + ':');
        if (pos == std::string::npos ||
            (pos && line[pos - 1] != ' ' && line[pos - 1] != '\t'))
            continue;
        std::istringstream value(line.substr(pos + key.size() + 1));
        unsigned long long n = 0;
        std::string unit;
        if (!(value >> n >> unit) || unit != "kB" ||
            n > std::numeric_limits<std::size_t>::max() / 1024)
            throw std::runtime_error("invalid memory field: " + path.string() + ":" + key);
        return static_cast<std::size_t>(n) * 1024;
    }
    return std::nullopt;
}
inline std::optional<std::size_t> byte_value(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string text;
    if (!(input >> text) || text == "max")
        return std::nullopt;
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("invalid memory limit: " + path.string());
    std::size_t value = 0;
    for (char c : text) {
        const auto digit = static_cast<unsigned>(c - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10)
            throw std::runtime_error("memory limit overflow: " + path.string());
        value = value * 10 + digit;
    }
    return value;
}
inline bool list_contains(const std::string &list, unsigned id) {
    std::istringstream input(list);
    std::string part;
    while (std::getline(input, part, ',')) {
        const auto dash = part.find('-');
        const auto low = std::stoul(part.substr(0, dash));
        const auto high = dash == std::string::npos ? low : std::stoul(part.substr(dash + 1));
        if (low > high)
            throw std::runtime_error("invalid CPU/NUMA list");
        if (low <= id && id <= high)
            return true;
    }
    return false;
}
inline std::optional<std::size_t> node_available(const std::vector<unsigned> &cpus) {
    // Without a supplied CPU selection use the first permitted CPU, not all sockets.
    std::ifstream status("/proc/self/status");
    std::string line, cpu_list, mem_list;
    while (std::getline(status, line)) {
        if (line.rfind("Cpus_allowed_list:", 0) == 0)
            cpu_list = line.substr(line.find(':') + 1);
        if (line.rfind("Mems_allowed_list:", 0) == 0)
            mem_list = line.substr(line.find(':') + 1);
    }
    if (cpus.empty() && cpu_list.empty())
        return std::nullopt;
    const unsigned cpu = cpus.empty() ? static_cast<unsigned>(std::stoul(cpu_list)) : cpus.front();
    const std::filesystem::path root("/sys/devices/system/node");
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.size() <= 4 || name.rfind("node", 0) != 0 ||
            name.find_first_not_of("0123456789", 4) != std::string::npos)
            continue;
        std::ifstream input(it->path() / "cpulist");
        std::string members;
        if (!std::getline(input, members) || !list_contains(members, cpu))
            continue;
        const auto node = static_cast<unsigned>(std::stoul(name.substr(4)));
        if (!mem_list.empty() && !list_contains(mem_list, node))
            throw std::runtime_error("selected CPU NUMA node is outside allowed memory nodes");
        for (auto selected : cpus)
            if (!list_contains(members, selected))
                throw std::runtime_error("memory budget requires a single NUMA CPU set");
        const auto info = it->path() / "meminfo";
        const auto free = kib_field(info, "MemFree");
        if (!free)
            return std::nullopt;
        // Node meminfo has no MemAvailable. Count only half its non-shared file
        // pages as reclaimable, then apply the global headroom below as well.
        const auto files = kib_field(info, "FilePages").value_or(0);
        const auto shared = kib_field(info, "Shmem").value_or(0);
        const auto reclaim = (files > shared ? files - shared : 0) / 2;
        if (reclaim > std::numeric_limits<std::size_t>::max() - *free)
            throw std::runtime_error("node available memory overflow");
        return *free + reclaim;
    }
    return std::nullopt;
}
inline std::string mount_unescape(const std::string &value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 3 < value.size() &&
            value[i + 1] >= '0' && value[i + 1] <= '7' &&
            value[i + 2] >= '0' && value[i + 2] <= '7' &&
            value[i + 3] >= '0' && value[i + 3] <= '7') {
            result += static_cast<char>((value[i + 1] - '0') * 64 +
                                        (value[i + 2] - '0') * 8 + value[i + 3] - '0');
            i += 3;
        } else
            result += value[i];
    }
    return result;
}
inline bool has_word(const std::string &list, const std::string &word) {
    return ("," + list + ",").find("," + word + ",") != std::string::npos;
}
inline void cgroup_remaining(std::size_t &available) {
    std::ifstream membership("/proc/self/cgroup");
    std::string line, unified, legacy;
    while (std::getline(membership, line)) {
        const auto first = line.find(':'), second = line.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos)
            continue;
        const auto controllers = line.substr(first + 1, second - first - 1);
        if (controllers.empty())
            unified = line.substr(second + 1);
        else if (has_word(controllers, "memory"))
            legacy = line.substr(second + 1);
    }
    std::ifstream mounts("/proc/self/mountinfo");
    while (std::getline(mounts, line)) {
        const auto split = line.find(" - ");
        if (split == std::string::npos)
            continue;
        std::istringstream head(line.substr(0, split)), tail(line.substr(split + 3));
        std::string id, parent, device, root_text, mount_text, options, type, source, super;
        if (!(head >> id >> parent >> device >> root_text >> mount_text >> options) ||
            !(tail >> type >> source >> super))
            continue;
        const bool v2 = type == "cgroup2";
        if (!v2 && (type != "cgroup" || !has_word(super, "memory")))
            continue;
        const auto member = v2 ? unified : legacy;
        if (member.empty())
            continue;
        const std::filesystem::path root = mount_unescape(root_text);
        const std::filesystem::path mount = mount_unescape(mount_text);
        auto relative = std::filesystem::path(member).lexically_relative(root);
        bool safe = !relative.empty();
        for (const auto &part : relative)
            if (part == "..")
                safe = false;
        // In a cgroup namespace, / is the namespace root even when mountinfo
        // exposes its host subtree. The mount root itself remains a valid cap.
        auto group = safe ? (mount / relative).lexically_normal() : mount;
        for (;;) {
            const auto cap = byte_value(group / (v2 ? "memory.max" : "memory.limit_in_bytes"));
            if (cap) {
                const auto used = byte_value(group / (v2 ? "memory.current" : "memory.usage_in_bytes"));
                if (!used)
                    throw std::runtime_error("cannot read finite cgroup memory usage");
                available = std::min(available, *used < *cap ? *cap - *used : 0);
            }
            if (group == mount)
                break;
            const auto next = group.parent_path();
            if (next.empty() || next == group)
                break;
            group = next;
        }
    }
}
} // namespace resource_detail

// Snapshot for planning future allocations, not an OS reservation or peak RSS
// guarantee. Zero means automatic; a nonzero request is an additional ceiling.
inline std::size_t available_memory_budget(std::size_t requested_bytes = 0,
                                          const std::vector<unsigned> &cpus = {}) {
    using namespace resource_detail;
    const auto global = kib_field("/proc/meminfo", "MemAvailable");
    const auto node = node_available(cpus);
    if (!global || (!node && !requested_bytes))
        throw std::runtime_error("cannot discover available single-NUMA memory; set an explicit memory budget");
    auto available = *global;
    if (node)
        available = std::min(available, *node);
    for (int resource : {RLIMIT_AS, RLIMIT_DATA}) {
        rlimit limit{};
        if (getrlimit(resource, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
            const auto used = kib_field("/proc/self/status", resource == RLIMIT_AS ? "VmSize" : "VmData");
            if (!used)
                throw std::runtime_error("cannot read process memory usage");
            const auto cap = static_cast<std::size_t>(std::min<rlim_t>(
                limit.rlim_cur, std::numeric_limits<std::size_t>::max()));
            available = std::min(available, *used < cap ? cap - *used : 0);
        }
    }
    cgroup_remaining(available);
    // Leave 20% of discovered headroom for unaccounted allocations and other
    // activity. Explicit limits are ceilings, not a request to ignore real caps.
    available -= available / 5;
    if (requested_bytes)
        available = std::min(available, requested_bytes);
    if (available < 1024 * 1024)
        throw std::runtime_error("insufficient available memory for allocation budget");
    return available;
}
} // namespace dag
