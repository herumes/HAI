// Plain C++ with no pch.h, so tests/aitest can build it on its own (the project builds it without
// the precompiled header).
#include "aicalc.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace AiCalc {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    std::string err;

    explicit Parser(const std::string& text) : s(text) {}

    void Skip() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool Eat(char c) { Skip(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    bool Fail(const std::string& m) { if (err.empty()) err = m; return false; }

    bool Expr(double& v) {
        if (++depth > 40) return Fail("too deeply nested");
        const bool ok = Sum(v);
        --depth;
        return ok;
    }

    bool Sum(double& v) {
        if (!Product(v)) return false;
        for (;;) {
            Skip();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                const char op = s[i++];
                double r;
                if (!Product(r)) return false;
                v = op == '+' ? v + r : v - r;
            }
            else return true;
        }
    }

    bool Product(double& v) {
        if (!Power(v)) return false;
        for (;;) {
            Skip();
            if (i < s.size() && (s[i] == '*' || s[i] == '/' || s[i] == '%' || s[i] == 'x')) {
                const char op = s[i++];
                double r;
                if (!Power(r)) return false;
                if (op == '*' || op == 'x') v *= r;
                else if (r == 0) return Fail("division by zero");
                else if (op == '/') v /= r;
                else v = std::fmod(v, r);
            }
            else return true;
        }
    }

    bool Power(double& v) {
        if (!Unary(v)) return false;
        Skip();
        if (i < s.size() && s[i] == '^') {
            ++i;
            double r;
            if (!Power(r)) return false;      // right-associative
            v = std::pow(v, r);
        }
        return true;
    }

    bool Unary(double& v) {
        Skip();
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
            const char op = s[i++];
            if (++depth > 40) return Fail("too deeply nested");
            const bool ok = Unary(v);
            --depth;
            if (!ok) return false;
            if (op == '-') v = -v;
            return true;
        }
        return Atom(v);
    }

    bool Atom(double& v) {
        Skip();
        if (i >= s.size()) return Fail("unexpected end");
        if (s[i] == '(') {
            ++i;
            if (!Expr(v)) return false;
            if (!Eat(')')) return Fail("missing )");
            return true;
        }
        if (std::isdigit((unsigned char)s[i]) || s[i] == '.') {
            const char* b = s.c_str() + i;
            char* end = nullptr;
            v = std::strtod(b, &end);
            if (end == b) return Fail("bad number");
            i += (size_t)(end - b);
            return true;
        }
        if (std::isalpha((unsigned char)s[i])) {
            std::string name;
            while (i < s.size() && std::isalpha((unsigned char)s[i])) name += (char)std::tolower((unsigned char)s[i++]);
            if (name == "pi") { v = 3.14159265358979323846; return true; }
            if (name == "e") { v = 2.71828182845904523536; return true; }
            if (!Eat('(')) return Fail("unknown word " + name);
            std::vector<double> args;
            if (!Eat(')')) {
                for (;;) {
                    double a;
                    if (!Expr(a)) return false;
                    args.push_back(a);
                    if (args.size() > 8) return Fail("too many arguments");
                    if (Eat(',')) continue;
                    if (Eat(')')) break;
                    return Fail("missing )");
                }
            }
            auto need = [&](size_t n) { return args.size() == n ? true : Fail(name + " takes " + std::to_string(n) + " argument(s)"); };
            if (name == "sqrt") { if (!need(1)) return false; if (args[0] < 0) return Fail("sqrt of a negative"); v = std::sqrt(args[0]); }
            else if (name == "abs") { if (!need(1)) return false; v = std::fabs(args[0]); }
            else if (name == "floor") { if (!need(1)) return false; v = std::floor(args[0]); }
            else if (name == "ceil") { if (!need(1)) return false; v = std::ceil(args[0]); }
            else if (name == "round") { if (!need(1)) return false; v = std::round(args[0]); }
            else if (name == "ln") { if (!need(1)) return false; if (args[0] <= 0) return Fail("ln of a non-positive"); v = std::log(args[0]); }
            else if (name == "log") { if (!need(1)) return false; if (args[0] <= 0) return Fail("log of a non-positive"); v = std::log10(args[0]); }
            else if (name == "sin") { if (!need(1)) return false; v = std::sin(args[0]); }
            else if (name == "cos") { if (!need(1)) return false; v = std::cos(args[0]); }
            else if (name == "tan") { if (!need(1)) return false; v = std::tan(args[0]); }
            else if (name == "min") { if (!need(2)) return false; v = (std::min)(args[0], args[1]); }
            else if (name == "max") { if (!need(2)) return false; v = (std::max)(args[0], args[1]); }
            else return Fail("unknown function " + name);
            return true;
        }
        return Fail(std::string("unexpected '") + s[i] + "'");
    }
};

} // namespace

bool Eval(const std::string& expr, double& out, std::string& err) {
    err.clear();
    if (expr.size() > 200) { err = "expression too long"; return false; }
    Parser p(expr);
    double v = 0;
    if (!p.Expr(v)) { err = p.err.empty() ? "cannot read that" : p.err; return false; }
    p.Skip();
    if (p.i != expr.size()) { err = "unexpected text after the expression"; return false; }
    if (!std::isfinite(v)) { err = "result is not a finite number"; return false; }
    out = v;
    return true;
}

std::string FormatNumber(double v) {
    if (v == 0) return "0";
    char buf[64];
    if (std::fabs(v) < 1e15 && v == std::floor(v)) {
        snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string RollDice(const std::string& specIn, const std::function<unsigned(unsigned)>& roll) {
    std::string spec;
    for (char c : specIn) if (!std::isspace((unsigned char)c)) spec += (char)std::tolower((unsigned char)c);
    if (spec.empty()) return "error: say what to roll, like d20 or 2d6+3";
    size_t i = 0;
    auto number = [&](unsigned& out) {
        size_t start = i;
        unsigned long v = 0;
        while (i < spec.size() && std::isdigit((unsigned char)spec[i]) && i - start < 6) v = v * 10 + (unsigned long)(spec[i++] - '0');
        out = (unsigned)v;
        return i > start;
    };
    unsigned count = 1, sides = 0;
    const size_t before = i;
    if (!number(count)) count = 1;
    if (i >= spec.size() || spec[i] != 'd') {
        (void)before;
        return "error: use the form NdM, like 2d6+3";
    }
    ++i;
    if (!number(sides)) return "error: how many sides?";
    long long bonus = 0;
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-')) {
        const bool neg = spec[i++] == '-';
        unsigned k = 0;
        if (!number(k)) return "error: bonus is not a number";
        bonus = neg ? -(long long)k : (long long)k;
    }
    if (i != spec.size()) return "error: unexpected text in the roll";
    if (count < 1 || count > 100) return "error: roll between 1 and 100 dice";
    if (sides < 2 || sides > 1000) return "error: dice need 2 to 1000 sides";

    long long total = bonus;
    std::string list;
    for (unsigned k = 0; k < count; ++k) {
        const unsigned r = roll(sides);
        total += r;
        if (k) list += " + ";
        list += std::to_string(r);
    }
    std::string out = std::to_string(count) + "d" + std::to_string(sides);
    if (bonus > 0) out += "+" + std::to_string(bonus);
    else if (bonus < 0) out += std::to_string(bonus);
    out += ": " + list;
    if (bonus != 0) out += (bonus > 0 ? " + " : " - ") + std::to_string(bonus > 0 ? bonus : -bonus);
    out += " = " + std::to_string(total);
    return out;
}

} // namespace AiCalc
