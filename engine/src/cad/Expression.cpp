// Parametric expressions (see Phisiks/Cad/Expression.h): a recursive-descent parser.
#include "Phisiks/Cad/Expression.h"

#include <cctype>
#include <cmath>
#include <stdexcept>

namespace Phisiks::Cad {
    namespace {
        constexpr double PI = 3.14159265358979323846;
        const char* FUNCTIONS[] = { "sqrt", "abs", "min", "max", "sin", "cos", "tan", "asin", "acos", "atan" };

        struct Parser {
            const std::string& s;
            const ParameterLookup* lookup;
            std::vector<std::string>* names;
            size_t i = 0;
            int depth = 0;

            [[noreturn]] void Fail(const std::string& m) { throw std::runtime_error(m + " at position " + std::to_string(i + 1)); }
            void Skip() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
            bool Eat(char c) { Skip(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

            double Expr() {
                if (++depth > 64) Fail("expression nested too deeply");
                double v = Term();
                while (true) {
                    if (Eat('+')) v += Term();
                    else if (Eat('-')) v -= Term();
                    else break;
                }
                --depth;
                return v;
            }
            double Term() {
                double v = Unary();
                while (true) {
                    if (Eat('*')) v *= Unary();
                    else if (Eat('/')) { const double d = Unary(); if (d == 0.0) Fail("division by zero"); v /= d; }
                    else break;
                }
                return v;
            }
            // unary minus binds looser than ^:  -w^2 = -(w^2),  2^-1 = 0.5
            double Unary() {
                if (Eat('-')) return -Unary();
                if (Eat('+')) return Unary();
                return Power();
            }
            double Power() {
                const double base = Primary();
                if (Eat('^')) return std::pow(base, Unary());   // right-associative
                return base;
            }
            double Primary() {
                Skip();
                if (i >= s.size()) Fail("unexpected end");
                if (Eat('(')) { const double v = Expr(); if (!Eat(')')) Fail("missing ')'"); return v; }
                const char c = s[i];
                if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
                    size_t used = 0;
                    double v;
                    try { v = std::stod(s.substr(i), &used); } catch (...) { Fail("bad number"); }
                    i += used;
                    return v;
                }
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                    const size_t start = i;
                    while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
                    const std::string name = s.substr(start, i - start);
                    Skip();
                    if (i < s.size() && s[i] == '(') {
                        ++i;
                        std::vector<double> args;
                        if (!Eat(')')) {
                            do args.push_back(Expr()); while (Eat(','));
                            if (!Eat(')')) Fail("missing ')'");
                        }
                        return Call(name, args);
                    }
                    if (name == "pi") return PI;
                    if (names) { names->push_back(name); return 1.0; }
                    double v = 0.0;
                    if (!lookup || !*lookup || !(*lookup)(name, v)) Fail("unknown parameter '" + name + "'");
                    return v;
                }
                Fail(std::string("unexpected '") + c + "'");
            }
            double Call(const std::string& f, const std::vector<double>& a) {
                auto need = [&](size_t n) { if (a.size() != n) Fail(f + "() takes " + std::to_string(n) + " argument(s)"); };
                const double deg = PI / 180.0;
                if (f == "sqrt") { need(1); if (a[0] < 0 && !names) Fail("sqrt of a negative number"); return std::sqrt(std::fabs(a[0])); }
                if (f == "abs") { need(1); return std::fabs(a[0]); }
                if (f == "min") { need(2); return std::fmin(a[0], a[1]); }
                if (f == "max") { need(2); return std::fmax(a[0], a[1]); }
                if (f == "sin") { need(1); return std::sin(a[0] * deg); }
                if (f == "cos") { need(1); return std::cos(a[0] * deg); }
                if (f == "tan") { need(1); return std::tan(a[0] * deg); }
                if (f == "asin") { need(1); return std::asin(a[0]) / deg; }
                if (f == "acos") { need(1); return std::acos(a[0]) / deg; }
                if (f == "atan") { need(1); return std::atan(a[0]) / deg; }
                Fail("unknown function '" + f + "'");
            }
        };
    }

    bool Evaluate(const std::string& expression, const ParameterLookup& lookup, double& out, std::string* error) {
        try {
            if (expression.size() > 4096) throw std::runtime_error("expression too long");
            Parser p{ expression, &lookup, nullptr };
            const double v = p.Expr();
            p.Skip();
            if (p.i != expression.size()) p.Fail("unexpected text");
            if (!std::isfinite(v)) throw std::runtime_error("result is not a finite number");
            out = v;
            return true;
        }
        catch (const std::exception& e) {
            if (error) *error = "'" + expression + "': " + e.what();
            return false;
        }
    }

    std::vector<std::string> ExpressionNames(const std::string& expression) {
        std::vector<std::string> names;
        try {
            Parser p{ expression, nullptr, &names };
            p.Expr();
        }
        catch (...) {}
        return names;
    }

    bool IsValidParameterName(const std::string& name) {
        if (name.empty() || name.size() > 64 || name == "pi") return false;
        if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;
        for (char c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        for (const char* f : FUNCTIONS) if (name == f) return false;
        return true;
    }
}
