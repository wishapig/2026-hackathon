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
// Fixed schema positions eliminate repeated field names, not program content.
// Reconstruct the ordinary Json representation so every existing semantic check
// remains shared by all container versions.
const Json::Array &compact_array(const Json &j, std::size_t size) {
    const auto &a = j.arr();
    if (a.size() != size)
        throw std::runtime_error("invalid compact metadata field count");
    return a;
}
Json compact_regions(const Json &j) {
    Json::Array result;
    for (const auto &item : j.arr()) {
        const auto &a = compact_array(item, 5);
        result.push_back(Json{Json::Object{{"buffer", a[0]}, {"offset", a[1]},
                                          {"rows", a[2]}, {"cols", a[3]}, {"stride", a[4]}}});
    }
    return Json{std::move(result)};
}
Json compact_nodes(const Json &j, unsigned depth = 0) {
    if (depth > 32)
        throw std::runtime_error("compact program nesting limit");
    Json::Array result;
    for (const auto &item : j.arr()) {
        const auto &a = item.arr();
        if (a.empty())
            throw std::runtime_error("empty compact program node");
        const auto tag = a[0].number();
        const auto expected = tag == 0 ? 3u : tag == 1 ? 6u : tag == 2 ? 4u :
                              tag == 3 ? 5u : (tag == 4 || tag == 5) ? 1u : 0u;
        if (!expected || a.size() != expected)
            throw std::runtime_error("invalid compact program tag or field count");
        Json::Object node;
        if (tag == 0) {
            node = {{"kind", Json{std::string("let")}}, {"name", a[1]}, {"value", a[2]}};
        } else if (tag == 1) {
            node = {{"kind", Json{std::string("for")}}, {"var", a[1]}, {"start", a[2]},
                    {"stop", a[3]}, {"step", a[4]}, {"body", compact_nodes(a[5], depth + 1)}};
        } else if (tag == 2) {
            node = {{"kind", Json{std::string("if")}}, {"condition", a[1]},
                    {"then", compact_nodes(a[2], depth + 1)},
                    {"else", compact_nodes(a[3], depth + 1)}};
        } else if (tag == 3) {
            node = {{"kind", Json{std::string("call")}}, {"op", a[1]}, {"args", a[2]},
                    {"reads", compact_regions(a[3])}, {"writes", compact_regions(a[4])}};
        } else {
            node = {{"kind", Json{std::string(tag == 4 ? "copy" : "zero_upper")}}};
        }
        result.push_back(Json{std::move(node)});
    }
    return Json{std::move(result)};
}
class BinaryParser {
    const std::string &s;
    std::size_t p = 0, nodes = 0, maximum;
    [[noreturn]] void fail() const {
        throw std::runtime_error("invalid binary metadata at byte " + std::to_string(p));
    }
    unsigned char take() {
        if (p == s.size())
            fail();
        return static_cast<unsigned char>(s[p++]);
    }
    std::uint64_t argument(unsigned tag) {
        if (tag < 24)
            return tag;
        if (tag > 27)
            fail();
        const unsigned bytes = 1u << (tag - 24);
        std::uint64_t result = 0;
        for (unsigned i = 0; i < bytes; ++i)
            result = (result << 8) | take();
        const auto minimum = bytes == 1 ? std::uint64_t(24) :
                             std::uint64_t(1) << (bytes * 4);
        if (result < minimum)
            fail();
        return result;
    }
    Json value(unsigned depth) {
        if (depth > 64 || ++nodes > maximum)
            throw std::runtime_error("binary metadata resource limit");
        const auto tag = take();
        const auto kind = tag >> 5;
        if (kind != 0 && kind != 1 && kind != 3 && kind != 4)
            fail();
        const auto n = argument(tag & 31);
        if (kind <= 1) {
            if (n > static_cast<std::uint64_t>(std::numeric_limits<Int>::max()))
                fail();
            return Json{kind == 0 ? static_cast<Int>(n) : -1 - static_cast<Int>(n)};
        }
        if (n > s.size() - p)
            fail();
        if (kind == 3) {
            if (n > 128)
                fail();
            std::string text;
            text.reserve(static_cast<std::size_t>(n));
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto c = take();
                if (c >= 128)
                    fail();
                text.push_back(static_cast<char>(c));
            }
            return Json{std::move(text)};
        }
        if (n > maximum - nodes)
            throw std::runtime_error("binary metadata array budget exceeded");
        Json::Array values;
        values.reserve(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i)
            values.push_back(value(depth + 1));
        return Json{std::move(values)};
    }

  public:
    BinaryParser(const std::string &bytes, std::size_t cap) : s(bytes), maximum(cap) {}
    Json parse() {
        auto result = value(0);
        if (p != s.size())
            fail();
        return result;
    }
};
Json compact_document(const std::string &payload, std::size_t maximum, bool binary = false) {
    if (payload.size() < 64)
        throw std::runtime_error("truncated compact input digests");
    const auto digest = [&](std::size_t offset) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (std::size_t i = offset; i < offset + 32; ++i) {
            const auto c = static_cast<unsigned char>(payload[i]);
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 15]);
        }
        return result;
    };
    const auto body = payload.substr(64);
    const auto parsed = binary ? BinaryParser(body, maximum).parse() : Parser(body, maximum).parse();
    const auto &a = compact_array(parsed, 4);
    const auto frontend = a[2].number();
    if (frontend != 0 && frontend != 1)
        throw std::runtime_error("invalid compact frontend semantics");
    Json::Object root{{"schema_version", Json{Int(1)}},
                      {"source_digest", Json{digest(0)}},
                      {"contract_digest", Json{digest(32)}},
                      {"operators", a[0]}, {"parameter_guards", a[1]},
                      {"program", compact_nodes(a[3])}};
    if (frontend)
        root.emplace("frontend", Json{std::string("strict-structured-cpp-v1")});
    return Json{std::move(root)};
}
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
    std::size_t bytes = sizeof(Program);
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

// Lean containers carry only semantics. The names below are internal identities
// for the existing lexical lowerer, not original source names or audit strings.
std::string lean_variable(Int slot) {
    if (slot < 0)
        throw std::runtime_error("invalid variable slot");
    return slot == 0 ? "n" : slot == 1 ? "b" : "v" + std::to_string(slot);
}
Expr lean_expression(const Json &j) {
    Expr e;
    if (const auto n = std::get_if<Int>(&j.v)) {
        e.op = "literal";
        e.value = *n;
        return e;
    }
    const auto &a = j.arr();
    if (a.empty())
        throw std::runtime_error("empty expression");
    const auto code = a[0].number();
    if (code == 0) {
        if (a.size() != 2) throw std::runtime_error("invalid variable reference");
        e.op = "variable";
        e.name = lean_variable(a[1].number());
        return e;
    }
    static const char *ops[] = {"neg", "!", "+", "-", "*", "/", "%",
        "<", "<=", ">", ">=", "==", "!=", "&&", "||"};
    if (code < 1 || code > 15 || a.size() != (code <= 2 ? 2u : 3u))
        throw std::runtime_error("invalid expression opcode");
    e.op = ops[code - 1];
    for (std::size_t i = 1; i < a.size(); ++i)
        e.args.push_back(lean_expression(a[i]));
    return e;
}
std::vector<RegionExpr> lean_regions(const Json &j, const std::vector<std::string> &buffers) {
    std::vector<RegionExpr> result;
    for (const auto &item : j.arr()) {
        const auto &a = compact_array(item, 5);
        const auto id = a[0].number();
        if (id < 0 || static_cast<std::size_t>(id) >= buffers.size())
            throw std::runtime_error("invalid buffer reference");
        result.push_back({buffers[static_cast<std::size_t>(id)], lean_expression(a[1]),
            lean_expression(a[2]), lean_expression(a[3]), lean_expression(a[4])});
    }
    return result;
}
std::vector<Node> lean_nodes(const Json &j, const std::vector<std::string> &operators,
                             const std::vector<std::string> &buffers) {
    std::vector<Node> result;
    for (const auto &item : j.arr()) {
        const auto &a = item.arr();
        if (a.empty()) throw std::runtime_error("empty statement");
        const auto tag = a[0].number();
        const auto count = tag == 0 ? 3u : tag == 1 ? 6u : tag == 2 ? 4u :
                           tag == 3 ? 5u : tag == 4 || tag == 5 ? 1u : 0u;
        if (!count || a.size() != count) throw std::runtime_error("invalid statement");
        Node n{};
        switch (tag) {
        case 0:
            n.kind = Node::Kind::Let; n.name = lean_variable(a[1].number());
            n.value = lean_expression(a[2]); break;
        case 1:
            n.kind = Node::Kind::For; n.name = lean_variable(a[1].number());
            n.start = lean_expression(a[2]); n.stop = lean_expression(a[3]);
            n.step = lean_expression(a[4]); n.body = lean_nodes(a[5], operators, buffers); break;
        case 2:
            n.kind = Node::Kind::If; n.condition = lean_expression(a[1]);
            n.body = lean_nodes(a[2], operators, buffers);
            n.otherwise = lean_nodes(a[3], operators, buffers); break;
        case 3: {
            n.kind = Node::Kind::Call;
            const auto id = a[1].number();
            if (id < 0 || static_cast<std::size_t>(id) >= operators.size())
                throw std::runtime_error("invalid operator reference");
            n.symbol = operators[static_cast<std::size_t>(id)];
            for (const auto &x : a[2].arr()) n.args.push_back(lean_expression(x));
            n.reads = lean_regions(a[3], buffers); n.writes = lean_regions(a[4], buffers); break;
        }
        case 4: n.kind = Node::Kind::Copy; break;
        case 5: n.kind = Node::Kind::ZeroUpper; break;
        }
        result.push_back(std::move(n));
    }
    return result;
}
Program load_lean(std::ifstream &in, std::size_t size, bool compressed, const Limits &limits) {
    std::size_t length = 0, header = 1;
    unsigned shift = 0;
    for (;;) {
        const auto byte = in.get();
        if (byte < 0 || shift >= 63 || static_cast<std::size_t>(byte & 127) >
            (std::numeric_limits<std::size_t>::max() >> shift))
            throw std::runtime_error("invalid metadata length");
        ++header;
        length |= static_cast<std::size_t>(byte & 127) << shift;
        if (!(byte & 128)) break;
        shift += 7;
    }
    if (header > size || !length) throw std::runtime_error("empty metadata");
    if (length > limits.memory_bytes / 32 || size > limits.memory_bytes / 32)
        throw ResourceLimit("metadata exceeds memory budget");
    std::string stored(size - header, '\0');
    in.read(stored.data(), static_cast<std::streamsize>(stored.size()));
    if (!in) throw std::runtime_error("truncated metadata");
    std::string payload;
    if (compressed) {
        if (length > std::numeric_limits<uInt>::max() || stored.size() > std::numeric_limits<uInt>::max())
            throw ResourceLimit("compressed metadata exceeds decoder capacity");
        if (limits.memory_bytes < 128 * 1024 ||
            length + stored.size() > limits.memory_bytes - 128 * 1024)
            throw ResourceLimit("metadata decoder workspace exceeds budget");
        payload.resize(length);
        z_stream stream{};
        stream.next_in = reinterpret_cast<Bytef *>(stored.data());
        stream.avail_in = static_cast<uInt>(stored.size());
        stream.next_out = reinterpret_cast<Bytef *>(payload.data());
        stream.avail_out = static_cast<uInt>(payload.size());
        const auto start = inflateInit2(&stream, -15);
        if (start == Z_MEM_ERROR) throw std::bad_alloc();
        if (start != Z_OK) throw std::runtime_error("cannot initialize metadata decoder");
        const auto status = inflate(&stream, Z_FINISH);
        const bool complete = status == Z_STREAM_END && stream.total_out == length && !stream.avail_in;
        inflateEnd(&stream);
        if (status == Z_MEM_ERROR) throw std::bad_alloc();
        if (!complete) throw std::runtime_error("invalid compressed metadata");
    } else {
        if (stored.size() != length) throw std::runtime_error("metadata length mismatch");
        payload = std::move(stored);
    }
    const auto parsed = BinaryParser(payload, limits.memory_bytes / 256).parse();
    const auto &a = compact_array(parsed, 5);
    Program result;
    result.source_int32 = a[0].number() != 0;
    for (const auto &x : a[1].arr()) result.operators.push_back(x.str());
    std::vector<std::string> buffers;
    for (const auto &x : a[2].arr()) buffers.push_back(x.str());
    for (const auto &x : a[3].arr()) result.parameter_guards.push_back(lean_expression(x));
    result.nodes = lean_nodes(a[4], result.operators, buffers);
    result.artifact_bytes = size;
    result.retained_bytes = program_bytes(result);
    if (result.retained_bytes > limits.memory_bytes)
        throw ResourceLimit("decoded metadata exceeds memory budget");
    return result;
}
// Lower once per enumeration, then use integer opcodes and lexical slots in
// the hot loop. The public Program stays immutable and remains the source of
// diagnostics. In particular this does not cache annotations in mutable Exprs.
enum class Code {
    Invalid, Literal, Variable, Neg, Not, Add, Sub, Mul, Div, Mod,
    Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, And, Or
};
Code opcode(const std::string &op) {
    if (op == "literal") return Code::Literal;
    if (op == "variable") return Code::Variable;
    if (op == "neg") return Code::Neg;
    if (op == "!") return Code::Not;
    if (op == "+") return Code::Add;
    if (op == "-") return Code::Sub;
    if (op == "*") return Code::Mul;
    if (op == "/") return Code::Div;
    if (op == "%") return Code::Mod;
    if (op == "<") return Code::Less;
    if (op == "<=") return Code::LessEqual;
    if (op == ">") return Code::Greater;
    if (op == ">=") return Code::GreaterEqual;
    if (op == "==") return Code::Equal;
    if (op == "!=") return Code::NotEqual;
    if (op == "&&") return Code::And;
    if (op == "||") return Code::Or;
    return Code::Invalid;
}
constexpr std::size_t no_slot = std::numeric_limits<std::size_t>::max();
struct BoundExpr {
    const Expr *source = nullptr;
    Code code = Code::Invalid;
    std::size_t slot = no_slot;
    std::vector<BoundExpr> args;
};
struct BoundRegion {
    BoundExpr offset, rows, cols, stride;
};
struct BoundNode {
    const Node *source = nullptr;
    std::size_t slot = no_slot;
    BoundExpr value, start, stop, step, condition;
    std::vector<BoundExpr> args;
    std::vector<BoundRegion> reads, writes;
    std::vector<BoundNode> body, otherwise;
    Task scratch;
};
struct BoundProgram {
    std::vector<BoundNode> nodes;
    std::vector<BoundExpr> guards;
    std::size_t slots = 2, bytes = 0;
};
struct Lowering {
    // Symbol strings refer to the unchanged source tree, so copying a lexical
    // scope during this one-time traversal never copies/allocates variable names.
    using Symbols = std::vector<std::pair<const std::string *, std::size_t>>;
    const Limits &limits;
    BoundProgram result;
    void take(std::size_t bytes) {
        // Double-charge retained storage as an allocator and temporary-scope
        // allowance, consistently with decoded Metadata's conservative budget.
        if (bytes > limits.memory_bytes / 2 ||
            result.bytes > limits.memory_bytes - bytes * 2)
            throw ResourceLimit("bound program storage exceeds memory budget");
        result.bytes += bytes * 2;
    }
    template<class T> void resize(std::vector<T> &v, std::size_t n) {
        if (n > limits.memory_bytes / sizeof(T))
            throw ResourceLimit("bound program array exceeds memory budget");
        take(n * sizeof(T));
        v.resize(n);
    }
    BoundExpr expression(const Expr &e, const Symbols &symbols) {
        BoundExpr out;
        out.source = &e;
        out.code = opcode(e.op);
        if (out.code == Code::Variable) {
            for (auto it = symbols.rbegin(); it != symbols.rend(); ++it)
                if (*it->first == e.name) {
                    out.slot = it->second;
                    break;
                }
            // An unbound variable inside an unexecuted branch/short-circuit
            // expression remains harmless, as with the source interpreter.
        }
        resize(out.args, e.args.size());
        for (std::size_t i = 0; i < e.args.size(); ++i)
            out.args[i] = expression(e.args[i], symbols);
        return out;
    }
    std::size_t bind(Symbols &symbols, const std::string &name) {
        if (result.slots == no_slot)
            throw std::runtime_error("bound variable slot overflow");
        take(sizeof(Int) + sizeof(Symbols::value_type));
        const auto slot = result.slots++;
        symbols.emplace_back(&name, slot);
        return slot;
    }
    std::vector<BoundNode> nodes(const std::vector<Node> &source, Symbols symbols) {
        take(symbols.size() * sizeof(Symbols::value_type));
        std::vector<BoundNode> output;
        resize(output, source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            const auto &n = source[i];
            auto &o = output[i];
            o.source = &n;
            switch (n.kind) {
            case Node::Kind::Let:
                o.value = expression(n.value, symbols);
                o.slot = bind(symbols, n.name);
                break;
            case Node::Kind::For: {
                o.start = expression(n.start, symbols);
                o.stop = expression(n.stop, symbols);
                o.step = expression(n.step, symbols);
                Symbols local = symbols;
                o.slot = bind(local, n.name);
                o.body = nodes(n.body, std::move(local));
                break;
            }
            case Node::Kind::If:
                o.condition = expression(n.condition, symbols);
                o.body = nodes(n.body, symbols);
                o.otherwise = nodes(n.otherwise, symbols);
                break;
            case Node::Kind::Call:
                resize(o.args, n.args.size());
                for (std::size_t j = 0; j < n.args.size(); ++j)
                    o.args[j] = expression(n.args[j], symbols);
                o.scratch.symbol = n.symbol;
                take(o.scratch.symbol.capacity() + 1);
                resize(o.scratch.args, n.args.size());
                for (bool writing : {false, true}) {
                    const auto &source_regions = writing ? n.writes : n.reads;
                    auto &regions = writing ? o.writes : o.reads;
                    auto &scratch = writing ? o.scratch.writes : o.scratch.reads;
                    resize(regions, source_regions.size());
                    resize(scratch, source_regions.size());
                    for (std::size_t j = 0; j < source_regions.size(); ++j) {
                        const auto &r = source_regions[j];
                        auto &region = regions[j];
                        region.offset = expression(r.offset, symbols);
                        region.rows = expression(r.rows, symbols);
                        region.cols = expression(r.cols, symbols);
                        region.stride = expression(r.stride, symbols);
                        scratch[j].buffer = r.buffer;
                        take(scratch[j].buffer.capacity() + 1);
                    }
                }
                break;
            case Node::Kind::Copy:
            case Node::Kind::ZeroUpper:
                break;
            }
        }
        return output;
    }
    BoundProgram lower(const Program &program) {
        static const std::string n = "n", b = "b";
        take(sizeof(BoundProgram) + 2 * sizeof(Int) + 2 * sizeof(Symbols::value_type));
        Symbols parameters{{&n, 0}, {&b, 1}};
        resize(result.guards, program.parameter_guards.size());
        for (std::size_t i = 0; i < result.guards.size(); ++i)
            result.guards[i] = expression(program.parameter_guards[i], parameters);
        result.nodes = nodes(program.nodes, parameters);
        return std::move(result);
    }
};
struct Evaluator {
    Parameters p;
    const Limits &limits;
    const std::function<void(const Task &)> &emit;
    std::size_t steps = 0, count = 0, elements;
    bool source_int32 = false;
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
    Int eval(const BoundExpr &e, const std::vector<Int> &values) {
        tick();
        if (e.code == Code::Literal)
            return checked(e.source->value);
        if (e.code == Code::Variable) {
            if (e.slot == no_slot)
                throw std::runtime_error("unbound variable: " + e.source->name);
            return values[e.slot];
        }
        Int a = eval(e.args.at(0), values);
        if (e.code == Code::Neg)
            return checked(-static_cast<__int128>(a));
        if (e.code == Code::Not)
            return !a;
        if (e.code == Code::And && !a)
            return 0;
        if (e.code == Code::Or && a)
            return 1;
        Int b = eval(e.args.at(1), values);
        switch (e.code) {
        case Code::Add: return checked(static_cast<__int128>(a) + b);
        case Code::Sub: return checked(static_cast<__int128>(a) - b);
        case Code::Mul: return checked(static_cast<__int128>(a) * b);
        case Code::Div:
        case Code::Mod:
            if (b == 0)
                throw std::runtime_error("division by zero");
            if ((a == std::numeric_limits<Int>::min() ||
                 (source_int32 && a == std::numeric_limits<std::int32_t>::min())) && b == -1)
                throw std::runtime_error("integer division overflow");
            return checked(e.code == Code::Div ? a / b : a % b);
        case Code::Less: return a < b;
        case Code::LessEqual: return a <= b;
        case Code::Greater: return a > b;
        case Code::GreaterEqual: return a >= b;
        case Code::Equal: return a == b;
        case Code::NotEqual: return a != b;
        case Code::And: return a && b;
        case Code::Or: return a || b;
        default: throw std::runtime_error("invalid evaluated operator");
        }
    }
    std::size_t nonneg(const BoundExpr &e, const std::vector<Int> &values) {
        Int x = eval(e, values);
        if (x < 0)
            throw std::runtime_error("negative memory region");
        return static_cast<std::size_t>(x);
    }
    void region(const BoundRegion &e, const std::vector<Int> &values, Region &r) {
        r.offset = nonneg(e.offset, values);
        r.rows = nonneg(e.rows, values);
        r.cols = nonneg(e.cols, values);
        r.stride = nonneg(e.stride, values);
        if (r.cols > r.stride)
            throw std::runtime_error("region columns exceed stride");
        __int128 end = r.offset;
        if (r.rows && r.cols)
            end += static_cast<__int128>(r.rows - 1) * r.stride + r.cols;
        if (end > elements)
            throw std::runtime_error("memory region out of bounds");
    }
    void publish(const Task &t) {
        if (count == std::numeric_limits<std::size_t>::max() ||
            (limits.max_tasks && count >= limits.max_tasks))
            throw std::runtime_error("task count budget exceeded");
        ++count;
        emit(t);
    }
    void run(std::vector<BoundNode> &nodes, std::vector<Int> &values) {
        for (auto &node : nodes) {
            tick();
            switch (node.source->kind) {
            case Node::Kind::Let:
                values[node.slot] = checked(eval(node.value, values));
                break;
            case Node::Kind::For: {
                Int first = checked(eval(node.start, values)), last = eval(node.stop, values);
                // C++ never evaluates the increment of a zero-iteration loop.
                // The supported step is invariant, but need not be defined when
                // the loop body is unreachable for the actual parameters.
                if (first >= last)
                    break;
                Int step = eval(node.step, values);
                if (step <= 0)
                    throw std::runtime_error("nonpositive loop step");
                for (Int i = first; i < last;) {
                    tick();
                    values[node.slot] = i;
                    run(node.body, values);
                    i = checked(static_cast<__int128>(i) + step);
                }
                break;
            }
            case Node::Kind::If: {
                run(eval(node.condition, values) ? node.body : node.otherwise, values);
                break;
            }
            case Node::Kind::Call: {
                auto &t = node.scratch;
                for (std::size_t i = 0; i < node.args.size(); ++i)
                    t.args[i] = eval(node.args[i], values);
                for (std::size_t i = 0; i < node.reads.size(); ++i)
                    region(node.reads[i], values, t.reads[i]);
                for (std::size_t i = 0; i < node.writes.size(); ++i)
                    region(node.writes[i], values, t.writes[i]);
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
    if (size >= 2) {
        in.seekg(0);
        const auto marker = in.get();
        if (marker == 0xa7 || marker == 0xa8)
            return load_lean(in, static_cast<std::size_t>(size), marker == 0xa8, limits);
    }
    // Reserve space for parser/tree overhead as well as source bytes.
    if (size < 24 || static_cast<std::uint64_t>(size) > 64ull * 1024 * 1024)
        throw std::runtime_error("metadata size violation");
    if (static_cast<std::uint64_t>(size) > limits.memory_bytes / 32)
        throw ResourceLimit("metadata storage exceeds memory budget");
    in.seekg(0);
    std::array<unsigned char, 24> h{};
    in.read(reinterpret_cast<char *>(h.data()), 24);
    if (!in)
        throw std::runtime_error("truncated metadata header");
    const std::string magic(reinterpret_cast<char *>(h.data()), 8);
    const bool binary = magic == "DAGMETA5" || magic == "DAGMETA6";
    const bool compressed = magic == "DAGMETA2" || magic == "DAGMETA4" || magic == "DAGMETA6";
    const bool compact = magic == "DAGMETA3" || magic == "DAGMETA4" || binary;
    if (magic != "DAGMETA1" && !compressed && !compact)
        throw std::runtime_error("metadata magic/version mismatch");
    const auto length = little(h.data() + 8);
    const auto stored_length = static_cast<std::uint64_t>(size) - 24;
    if (length > 64ull * 1024 * 1024)
        throw std::runtime_error("decoded metadata size violation");
    if (length > limits.memory_bytes / 32)
        throw ResourceLimit("decoded metadata exceeds memory budget");
    std::string payload;
    if (!compressed) {
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
            throw ResourceLimit("compressed metadata workspace exceeds memory budget");
        std::string stored(static_cast<std::size_t>(stored_length), '\0');
        in.read(stored.data(), static_cast<std::streamsize>(stored_length));
        if (!in)
            throw std::runtime_error("truncated compressed metadata");
        payload.resize(static_cast<std::size_t>(length));
        auto decoded = static_cast<uLongf>(length);
        auto consumed = static_cast<uLong>(stored_length);
        const int status = uncompress2(reinterpret_cast<Bytef *>(payload.data()), &decoded,
                                       reinterpret_cast<const Bytef *>(stored.data()), &consumed);
        if (status == Z_MEM_ERROR)
            throw ResourceLimit("zlib workspace allocation failed");
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
    Json root = compact ? compact_document(payload, limits.memory_bytes / 512, binary)
                        : Parser(payload, limits.memory_bytes / 256).parse();
    keys(root, {"schema_version", "source_digest", "contract_digest", "operators", "program",
                "frontend", "parameter_guards"});
    if (root.at("schema_version").number() != 1)
        throw std::runtime_error("unsupported metadata schema");
    Program result;
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
        throw ResourceLimit("decoded metadata exceeds memory budget");
    return result;
}
namespace {
std::size_t parameter_elements(const Program &program, Parameters p) {
    if (p.n <= 0 || p.b <= 0 || p.b > p.n || p.n % p.b)
        throw std::runtime_error("require n>0, 0<b<=n, n%b==0");
    const auto elements = static_cast<__int128>(p.n) * p.n;
    if (elements > std::numeric_limits<Int>::max() ||
        elements > std::numeric_limits<std::size_t>::max() / sizeof(double))
        throw std::runtime_error("matrix extent overflow");
    if (program.source_int32 && (p.n > std::numeric_limits<std::int32_t>::max() ||
                                 p.b > std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("source int parameter overflow");
    return static_cast<std::size_t>(elements);
}
void execute_bound(const Program &program, BoundProgram &bound, Parameters p,
                   std::size_t elements, const Limits &limits,
                   const std::function<void(const Task &)> &emit) {
    Limits remaining = limits;
    remaining.memory_bytes -= bound.bytes;
    std::vector<Int> values(bound.slots, 0);
    values[0] = p.n;
    values[1] = p.b;
    Evaluator e{p, remaining, emit, 0, 0, elements, program.source_int32};
    for (const auto &guard : bound.guards)
        if (!e.eval(guard, values))
            throw std::runtime_error("source parameter guard rejected input");
    e.run(bound.nodes, values);
}
} // namespace
void enumerate_program(const Program &program, Parameters p, const Limits &limits,
                       const std::function<void(const Task &)> &emit) {
    const auto elements = parameter_elements(program, p);
    auto bound = Lowering{limits, {}}.lower(program);
    execute_bound(program, bound, p, elements, limits, emit);
}
std::vector<Task> instantiate(const Program &program, Parameters p, const Limits &limits) {
    const auto elements = parameter_elements(program, p);
    auto bound = Lowering{limits, {}}.lower(program);
    const auto available = limits.memory_bytes - bound.bytes;
    std::vector<Task> tasks;
    std::size_t bytes = 0;
    execute_bound(program, bound, p, elements, limits, [&](const Task &t) {
        std::size_t cost = sizeof(Task) + t.symbol.size() + 1 + t.args.size() * sizeof(Int);
        for (auto &r : t.reads)
            cost += sizeof(Region) + r.buffer.size() + 1;
        for (auto &r : t.writes)
            cost += sizeof(Region) + r.buffer.size() + 1;
        if (cost > available / 2 || bytes > available / 2 - cost)
            throw ResourceLimit("task storage exceeds memory budget");
        bytes += cost;
        tasks.push_back(t);
    });
    return tasks;
}
} // namespace dag
