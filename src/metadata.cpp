#include "dag/core.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <variant>
#include <zlib.h>

namespace dag {
namespace {
struct Json {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, Int, std::string, Array, Object> v;
    const Object &obj() const {
        if (auto p = std::get_if<Object>(&v))
            return *p;
        throw std::runtime_error("metadata: expected object");
    }
    const Array &arr() const {
        if (auto p = std::get_if<Array>(&v))
            return *p;
        throw std::runtime_error("metadata: expected array");
    }
    const std::string &str() const {
        if (auto p = std::get_if<std::string>(&v))
            return *p;
        throw std::runtime_error("metadata: expected string");
    }
    Int number() const {
        if (auto p = std::get_if<Int>(&v))
            return *p;
        throw std::runtime_error("metadata: expected integer");
    }
    const Json &at(const std::string &k) const {
        auto &o = obj();
        auto i = o.find(k);
        if (i == o.end())
            throw std::runtime_error("metadata: missing field " + k);
        return i->second;
    }
    bool has(const std::string &k) const {
        return obj().count(k) != 0;
    }
};
class Parser {
    const std::string &s;
    std::size_t p = 0, nodes = 0, maximum;
    [[noreturn]] void fail() const {
        throw std::runtime_error("invalid metadata JSON at byte " + std::to_string(p));
    }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t'))
            ++p;
    }
    char take() {
        if (p >= s.size())
            fail();
        return s[p++];
    }
    std::string string() {
        if (take() != '"')
            fail();
        std::string r;
        while (true) {
            unsigned char c = static_cast<unsigned char>(take());
            if (c == '"')
                break;
            if (c < 32)
                fail();
            if (c == '\\') {
                char e = take();
                switch (e) {
                case '"':
                    r += '"';
                    break;
                case '\\':
                    r += '\\';
                    break;
                case '/':
                    r += '/';
                    break;
                case 'b':
                    r += '\b';
                    break;
                case 'f':
                    r += '\f';
                    break;
                case 'n':
                    r += '\n';
                    break;
                case 'r':
                    r += '\r';
                    break;
                case 't':
                    r += '\t';
                    break;
                case 'u': {
                    unsigned u = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = take();
                        u *= 16;
                        if (h >= '0' && h <= '9')
                            u += h - '0';
                        else if (h >= 'a' && h <= 'f')
                            u += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            u += h - 'A' + 10;
                        else
                            fail();
                    }
                    if (u >= 0xd800 && u <= 0xdfff)
                        fail();
                    if (u < 128)
                        r += char(u);
                    else if (u < 2048) {
                        r += char(0xc0 | (u >> 6));
                        r += char(0x80 | (u & 63));
                    } else {
                        r += char(0xe0 | (u >> 12));
                        r += char(0x80 | ((u >> 6) & 63));
                        r += char(0x80 | (u & 63));
                    }
                    break;
                }
                default:
                    fail();
                }
            } else
                r += char(c);
        }
        return r;
    }
    Json value(unsigned depth) {
        if (depth > 64 || ++nodes > maximum)
            throw std::runtime_error("metadata JSON resource limit");
        ws();
        if (p == s.size())
            fail();
        char c = s[p];
        if (c == '"')
            return Json{string()};
        if (c == '{') {
            ++p;
            Json::Object o;
            ws();
            if (p < s.size() && s[p] == '}') {
                ++p;
                return Json{o};
            }
            while (true) {
                ws();
                auto k = string();
                ws();
                if (take() != ':')
                    fail();
                auto v = value(depth + 1);
                if (!o.emplace(k, std::move(v)).second)
                    throw std::runtime_error("metadata duplicate key");
                ws();
                char e = take();
                if (e == '}')
                    break;
                if (e != ',')
                    fail();
            }
            return Json{std::move(o)};
        }
        if (c == '[') {
            ++p;
            Json::Array a;
            ws();
            if (p < s.size() && s[p] == ']') {
                ++p;
                return Json{a};
            }
            while (true) {
                a.push_back(value(depth + 1));
                ws();
                char e = take();
                if (e == ']')
                    break;
                if (e != ',')
                    fail();
            }
            return Json{std::move(a)};
        }
        for (auto word : {"true", "false", "null"}) {
            std::string w = word;
            if (s.compare(p, w.size(), w) == 0) {
                p += w.size();
                if (w == "null")
                    return Json{nullptr};
                return Json{w == "true"};
            }
        }
        bool neg = false;
        if (c == '-') {
            neg = true;
            ++p;
        }
        if (p == s.size() || s[p] < '0' || s[p] > '9')
            fail();
        std::uint64_t u = 0;
        auto start = p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
            unsigned digit = unsigned(s[p++] - '0');
            if (u > (std::uint64_t(std::numeric_limits<Int>::max()) + unsigned(neg) - digit) / 10)
                fail();
            u = u * 10 + digit;
        }
        if (p - start > 1 && s[start] == '0')
            fail();
        Int x;
        if (neg && u == (std::uint64_t(1) << 63))
            x = std::numeric_limits<Int>::min();
        else
            x = neg ? -static_cast<Int>(u) : static_cast<Int>(u);
        return Json{x};
    }

  public:
    Parser(const std::string &x, std::size_t cap) : s(x), maximum(cap) {}
    Json parse() {
        Json j = value(0);
        ws();
        if (p != s.size())
            fail();
        return j;
    }
};
void keys(const Json &j, std::initializer_list<const char *> valid) {
    for (const auto &kv : j.obj()) {
        bool found = false;
        for (auto k : valid)
            if (kv.first == k)
                found = true;
        if (!found)
            throw std::runtime_error("metadata unknown field: " + kv.first);
    }
}
bool identifier(const std::string &s) {
    if (s.empty() || s.size() > 128)
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i && c >= '0' && c <= '9')))
            return false;
    }
    return true;
}
Expr expression(const Json &j, unsigned depth = 0) {
    if (depth > 48)
        throw std::runtime_error("expression depth limit");
    Expr e;
    if (auto n = std::get_if<Int>(&j.v)) {
        e.op = "literal";
        e.value = *n;
        return e;
    }
    if (auto s = std::get_if<std::string>(&j.v)) {
        if (!identifier(*s))
            throw std::runtime_error("invalid variable");
        e.op = "variable";
        e.name = *s;
        return e;
    }
    auto &a = j.arr();
    if (a.empty())
        throw std::runtime_error("empty expression");
    e.op = a[0].str();
    static const std::set<std::string> binary = {
        "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "||"};
    unsigned arity = (e.op == "neg" || e.op == "!") ? 1 : binary.count(e.op) ? 2 : 0;
    if (!arity || a.size() != arity + 1)
        throw std::runtime_error("invalid expression operator/arity: " + e.op);
    for (std::size_t i = 1; i < a.size(); ++i)
        e.args.push_back(expression(a[i], depth + 1));
    return e;
}
std::vector<RegionExpr> region_exprs(const Json &j) {
    std::vector<RegionExpr> r;
    for (auto &x : j.arr()) {
        keys(x, {"buffer", "offset", "rows", "cols", "stride"});
        RegionExpr a;
        a.buffer = x.at("buffer").str();
        if (!identifier(a.buffer))
            throw std::runtime_error("invalid region buffer");
        a.offset = expression(x.at("offset"));
        a.rows = expression(x.at("rows"));
        a.cols = expression(x.at("cols"));
        a.stride = expression(x.at("stride"));
        r.push_back(std::move(a));
    }
    return r;
}
std::vector<Node> parse_nodes(const Json &j, const std::set<std::string> &ops, unsigned depth = 0) {
    if (depth > 32)
        throw std::runtime_error("program nesting limit");
    std::vector<Node> result;
    for (auto &x : j.arr()) {
        auto kind = x.at("kind").str();
        Node n;
        if (kind == "let") {
            keys(x, {"kind", "name", "value"});
            n.kind = Node::Kind::Let;
            n.name = x.at("name").str();
            n.value = expression(x.at("value"));
        } else if (kind == "for") {
            keys(x, {"kind", "var", "start", "stop", "step", "body"});
            n.kind = Node::Kind::For;
            n.name = x.at("var").str();
            n.start = expression(x.at("start"));
            n.stop = expression(x.at("stop"));
            n.step = expression(x.at("step"));
            n.body = parse_nodes(x.at("body"), ops, depth + 1);
        } else if (kind == "if") {
            keys(x, {"kind", "condition", "then", "else"});
            n.kind = Node::Kind::If;
            n.condition = expression(x.at("condition"));
            n.body = parse_nodes(x.at("then"), ops, depth + 1);
            if (x.has("else"))
                n.otherwise = parse_nodes(x.at("else"), ops, depth + 1);
        } else if (kind == "call") {
            keys(x, {"kind", "op", "args", "reads", "writes"});
            n.kind = Node::Kind::Call;
            n.symbol = x.at("op").str();
            if (!ops.count(n.symbol))
                throw std::runtime_error("call not declared in operator table");
            for (auto &a : x.at("args").arr())
                n.args.push_back(expression(a));
            n.reads = region_exprs(x.at("reads"));
            n.writes = region_exprs(x.at("writes"));
        } else if (kind == "copy" || kind == "zero_upper") {
            keys(x, {"kind"});
            n.kind = kind == "copy" ? Node::Kind::Copy : Node::Kind::ZeroUpper;
        } else
            throw std::runtime_error("unknown program node: " + kind);
        if (!n.name.empty() && (!identifier(n.name) || n.name == "n" || n.name == "b"))
            throw std::runtime_error("invalid or immutable binding: " + n.name);
        result.push_back(std::move(n));
    }
    return result;
}
std::size_t expression_bytes(const Expr &e) {
    std::size_t bytes = e.op.capacity() + e.name.capacity() + 2 + e.args.capacity() * sizeof(Expr);
    for (const auto &a : e.args)
        bytes += expression_bytes(a);
    return bytes;
}
std::size_t node_bytes(const Node &n) {
    std::size_t bytes = n.name.capacity() + n.symbol.capacity() + 2;
    for (const Expr *e : {&n.value, &n.start, &n.stop, &n.step, &n.condition})
        bytes += expression_bytes(*e);
    bytes += n.args.capacity() * sizeof(Expr);
    for (const auto &e : n.args)
        bytes += expression_bytes(e);
    for (const auto *rs : {&n.reads, &n.writes}) {
        bytes += rs->capacity() * sizeof(RegionExpr);
        for (const auto &r : *rs) {
            bytes += r.buffer.capacity() + 1;
            for (const Expr *e : {&r.offset, &r.rows, &r.cols, &r.stride})
                bytes += expression_bytes(*e);
        }
    }
    for (const auto *ns : {&n.body, &n.otherwise}) {
        bytes += ns->capacity() * sizeof(Node);
        for (const auto &child : *ns)
            bytes += node_bytes(child);
    }
    return bytes;
}
std::size_t program_bytes(const Program &p) {
    std::size_t bytes =
        sizeof(Program) + p.source_digest.capacity() + p.contract_digest.capacity() + 2;
    bytes += p.nodes.capacity() * sizeof(Node) + p.operators.capacity() * sizeof(std::string) +
             p.parameter_guards.capacity() * sizeof(Expr);
    for (const auto &op : p.operators)
        bytes += op.capacity() + 1;
    for (const auto &node : p.nodes)
        bytes += node_bytes(node);
    for (const auto &guard : p.parameter_guards)
        bytes += expression_bytes(guard);
    // Include allocator bookkeeping allowance, deliberately count SSO capacity too.
    return bytes * 2;
}
std::uint64_t little(const unsigned char *p) {
    std::uint64_t x = 0;
    for (unsigned i = 0; i < 8; ++i)
        x |= std::uint64_t(p[i]) << (8 * i);
    return x;
}
Int narrow(__int128 x) {
    if (x < std::numeric_limits<Int>::min() || x > std::numeric_limits<Int>::max())
        throw std::runtime_error("integer expression overflow");
    return static_cast<Int>(x);
}
// Bindings are a small lexical scope. Flat storage avoids a tree allocation
// for every variable in every iteration, without changing shadowing semantics.
struct Env {
    std::vector<std::pair<std::string, Int>> values;
    Env() = default;
    Env(std::initializer_list<std::pair<std::string, Int>> init) : values(init) {}
    auto find(const std::string &name) const {
        return std::find_if(values.begin(), values.end(), [&](const auto &v) {
            return v.first == name;
        });
    }
    auto end() const { return values.end(); }
    Int &operator[](const std::string &name) {
        for (auto &v : values)
            if (v.first == name)
                return v.second;
        values.emplace_back(name, 0);
        return values.back().second;
    }
};
struct Evaluator {
    Parameters p;
    const Limits &limits;
    const std::function<void(const Task &)> &emit;
    std::size_t steps = 0, count = 0, elements;
    bool source_int32 = false;
    std::map<const Node *, Task> call_scratch{};
    Int checked(__int128 value) {
        Int result = narrow(value);
        if (source_int32 && (result < std::numeric_limits<std::int32_t>::min() ||
                             result > std::numeric_limits<std::int32_t>::max()))
            throw std::runtime_error("source int arithmetic or conversion overflow");
        return result;
    }
    void tick() {
        if (steps == std::numeric_limits<std::size_t>::max() ||
            (limits.max_steps && steps >= limits.max_steps))
            throw std::runtime_error("program work budget exceeded");
        ++steps;
    }
    Int eval(const Expr &e, const Env &env) {
        tick();
        if (e.op == "literal")
            return checked(e.value);
        if (e.op == "variable") {
            auto i = env.find(e.name);
            if (i == env.end())
                throw std::runtime_error("unbound variable: " + e.name);
            return i->second;
        }
        Int a = eval(e.args.at(0), env);
        if (e.op == "neg")
            return checked(-static_cast<__int128>(a));
        if (e.op == "!")
            return !a;
        if (e.op == "&&" && !a)
            return 0;
        if (e.op == "||" && a)
            return 1;
        Int b = eval(e.args.at(1), env);
        if (e.op == "+")
            return checked(static_cast<__int128>(a) + b);
        if (e.op == "-")
            return checked(static_cast<__int128>(a) - b);
        if (e.op == "*")
            return checked(static_cast<__int128>(a) * b);
        if (e.op == "/" || e.op == "%") {
            if (b == 0)
                throw std::runtime_error("division by zero");
            if ((a == std::numeric_limits<Int>::min() ||
                 (source_int32 && a == std::numeric_limits<std::int32_t>::min())) &&
                b == -1)
                throw std::runtime_error("integer division overflow");
            return checked(e.op == "/" ? a / b : a % b);
        }
        if (e.op == "<")
            return a < b;
        if (e.op == "<=")
            return a <= b;
        if (e.op == ">")
            return a > b;
        if (e.op == ">=")
            return a >= b;
        if (e.op == "==")
            return a == b;
        if (e.op == "!=")
            return a != b;
        if (e.op == "&&")
            return a && b;
        if (e.op == "||")
            return a || b;
        throw std::runtime_error("invalid evaluated operator");
    }
    std::size_t nonneg(const Expr &e, const Env &env) {
        Int x = eval(e, env);
        if (x < 0)
            throw std::runtime_error("negative memory region");
        return static_cast<std::size_t>(x);
    }
    Region region(const RegionExpr &e, const Env &env) {
        Region r{e.buffer, nonneg(e.offset, env), nonneg(e.rows, env), nonneg(e.cols, env),
                 nonneg(e.stride, env)};
        if (r.cols > r.stride)
            throw std::runtime_error("region columns exceed stride");
        __int128 end = r.offset;
        if (r.rows && r.cols)
            end += static_cast<__int128>(r.rows - 1) * r.stride + r.cols;
        if (end > elements)
            throw std::runtime_error("memory region out of bounds");
        return r;
    }
    void publish(const Task &t) {
        if (count == std::numeric_limits<std::size_t>::max() ||
            (limits.max_tasks && count >= limits.max_tasks))
            throw std::runtime_error("task count budget exceeded");
        ++count;
        emit(t);
    }
    void run(const std::vector<Node> &nodes, Env &env) {
        for (auto &node : nodes) {
            tick();
            switch (node.kind) {
            case Node::Kind::Let:
                env[node.name] = checked(eval(node.value, env));
                break;
            case Node::Kind::For: {
                Int first = checked(eval(node.start, env)), last = eval(node.stop, env),
                    step = eval(node.step, env);
                if (step <= 0)
                    throw std::runtime_error("nonpositive loop step");
                Env local;
                for (Int i = first; i < last;) {
                    tick();
                    local = env;
                    local[node.name] = i;
                    run(node.body, local);
                    i = checked(static_cast<__int128>(i) + step);
                }
                break;
            }
            case Node::Kind::If: {
                Env local = env;
                run(eval(node.condition, env) ? node.body : node.otherwise, local);
                break;
            }
            case Node::Kind::Call: {
                auto &t = call_scratch[&node];
                t.symbol = node.symbol;
                t.args.resize(node.args.size());
                t.reads.resize(node.reads.size());
                t.writes.resize(node.writes.size());
                for (std::size_t i = 0; i < node.args.size(); ++i)
                    t.args[i] = eval(node.args[i], env);
                for (std::size_t i = 0; i < node.reads.size(); ++i)
                    t.reads[i] = region(node.reads[i], env);
                for (std::size_t i = 0; i < node.writes.size(); ++i)
                    t.writes[i] = region(node.writes[i], env);
                publish(t);
                break;
            }
            case Node::Kind::Copy: {
                Task t;
                t.symbol = "__copy";
                t.reads.push_back({"A", 0, static_cast<std::size_t>(p.n),
                                   static_cast<std::size_t>(p.n), static_cast<std::size_t>(p.n)});
                t.writes.push_back({"L", 0, static_cast<std::size_t>(p.n),
                                    static_cast<std::size_t>(p.n), static_cast<std::size_t>(p.n)});
                publish(std::move(t));
                break;
            }
            case Node::Kind::ZeroUpper: {
                Task t;
                t.symbol = "__zero_upper";
                if (static_cast<std::size_t>(p.n) > limits.memory_bytes / sizeof(Region))
                    throw ResourceLimit("zero-upper region budget");
                for (Int row = 0; row < p.n - 1; ++row)
                    t.writes.push_back({"L", static_cast<std::size_t>(row * p.n + row + 1), 1,
                                        static_cast<std::size_t>(p.n - row - 1),
                                        static_cast<std::size_t>(p.n)});
                publish(std::move(t));
                break;
            }
            }
        }
    }
};
} // namespace
Program load_metadata(const std::string &path, const Limits &limits) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("cannot open metadata: " + path);
    auto size = in.tellg();
    // Reserve space for parser/tree overhead as well as source bytes.
    if (size < 24 || static_cast<std::uint64_t>(size) >
                         std::min<std::size_t>(64ull * 1024 * 1024, limits.memory_bytes / 32))
        throw std::runtime_error("metadata size/budget violation");
    in.seekg(0);
    std::array<unsigned char, 24> h{};
    in.read(reinterpret_cast<char *>(h.data()), 24);
    if (!in)
        throw std::runtime_error("truncated metadata header");
    const std::string magic(reinterpret_cast<char *>(h.data()), 8);
    if (magic != "DAGMETA1" && magic != "DAGMETA2")
        throw std::runtime_error("metadata magic/version mismatch");
    const auto length = little(h.data() + 8);
    const auto stored_length = static_cast<std::uint64_t>(size) - 24;
    if (length > std::min<std::size_t>(64ull * 1024 * 1024, limits.memory_bytes / 32))
        throw std::runtime_error("decoded metadata size/budget violation");
    std::string payload;
    if (magic == "DAGMETA1") {
        if (length != stored_length)
            throw std::runtime_error("metadata length mismatch");
        payload.resize(static_cast<std::size_t>(length));
        in.read(payload.data(), static_cast<std::streamsize>(length));
        if (!in)
            throw std::runtime_error("truncated metadata");
    } else {
        // The declared decoded length is checked before allocation. uncompress2
        // additionally reports input consumption, so trailing bytes and appended
        // streams cannot be accepted as an otherwise valid artifact.
        if (!length || !stored_length ||
            length > std::numeric_limits<uLongf>::max() ||
            stored_length > std::numeric_limits<uLong>::max())
            throw std::runtime_error("invalid compressed metadata length");
        constexpr std::size_t inflate_reserve = 128 * 1024;
        if (limits.memory_bytes < inflate_reserve ||
            length > limits.memory_bytes - inflate_reserve ||
            stored_length > limits.memory_bytes - inflate_reserve - length)
            throw std::runtime_error("compressed metadata workspace exceeds memory budget");
        std::string stored(static_cast<std::size_t>(stored_length), '\0');
        in.read(stored.data(), static_cast<std::streamsize>(stored_length));
        if (!in)
            throw std::runtime_error("truncated compressed metadata");
        payload.resize(static_cast<std::size_t>(length));
        auto decoded = static_cast<uLongf>(length);
        auto consumed = static_cast<uLong>(stored_length);
        const int status = uncompress2(reinterpret_cast<Bytef *>(payload.data()), &decoded,
                                       reinterpret_cast<const Bytef *>(stored.data()), &consumed);
        if (status != Z_OK || decoded != length || consumed != stored_length)
            throw std::runtime_error("invalid compressed metadata stream or length");
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : payload) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    if (hash != little(h.data() + 16))
        throw std::runtime_error("metadata checksum mismatch");
    Json root = Parser(payload, limits.memory_bytes / 256).parse();
    keys(root, {"schema_version", "source_digest", "contract_digest", "operators", "program",
                "frontend", "parameter_guards"});
    if (root.at("schema_version").number() != 1)
        throw std::runtime_error("unsupported metadata schema");
    Program result;
    result.source_digest = root.at("source_digest").str();
    result.contract_digest = root.at("contract_digest").str();
    result.artifact_bytes = static_cast<std::size_t>(size);
    std::set<std::string> ops;
    for (auto &x : root.at("operators").arr()) {
        auto symbol = x.str();
        if (!identifier(symbol) || symbol.rfind("__", 0) == 0 || !ops.insert(symbol).second)
            throw std::runtime_error("invalid/duplicate operator");
        result.operators.push_back(symbol);
    }
    if (root.has("frontend")) {
        if (root.at("frontend").str() != "strict-structured-cpp-v1")
            throw std::runtime_error("unsupported source integer semantics");
        result.source_int32 = true;
    }
    if (root.has("parameter_guards"))
        for (const auto &guard : root.at("parameter_guards").arr())
            result.parameter_guards.push_back(expression(guard));
    result.nodes = parse_nodes(root.at("program"), ops);
    result.retained_bytes = program_bytes(result);
    if (result.retained_bytes > limits.memory_bytes)
        throw std::runtime_error("decoded metadata exceeds memory budget");
    return result;
}
void enumerate_program(const Program &program, Parameters p, const Limits &limits,
                       const std::function<void(const Task &)> &emit) {
    if (p.n <= 0 || p.b <= 0 || p.b > p.n || p.n % p.b)
        throw std::runtime_error("require n>0, 0<b<=n, n%b==0");
    auto elements = static_cast<__int128>(p.n) * p.n;
    if (elements > std::numeric_limits<Int>::max() ||
        elements > std::numeric_limits<std::size_t>::max() / sizeof(double))
        throw std::runtime_error("matrix extent overflow");
    if (program.source_int32 && (p.n > std::numeric_limits<std::int32_t>::max() ||
                                 p.b > std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("source int parameter overflow");
    Env env{{"n", p.n}, {"b", p.b}};
    Evaluator e{p, limits, emit, 0, 0, static_cast<std::size_t>(elements), program.source_int32};
    for (const auto &guard : program.parameter_guards)
        if (!e.eval(guard, env))
            throw std::runtime_error("source parameter guard rejected input");
    e.run(program.nodes, env);
}
std::vector<Task> instantiate(const Program &program, Parameters p, const Limits &limits) {
    std::vector<Task> tasks;
    std::size_t bytes = 0;
    enumerate_program(program, p, limits, [&](const Task &t) {
        std::size_t cost = sizeof(Task) + t.symbol.size() + 1 + t.args.size() * sizeof(Int);
        for (auto &r : t.reads)
            cost += sizeof(Region) + r.buffer.size() + 1;
        for (auto &r : t.writes)
            cost += sizeof(Region) + r.buffer.size() + 1;
        if (cost > limits.memory_bytes / 2 || bytes > limits.memory_bytes / 2 - cost)
            throw ResourceLimit("task storage exceeds memory budget");
        bytes += cost;
        tasks.push_back(t);
    });
    return tasks;
}
} // namespace dag
