#pragma once
// Arithmetic expressions for parametric values: numbers, parameter names, + - * / ^, parentheses,
// unary minus, pi, and sqrt abs min max sin cos tan (degrees) asin acos atan (degrees out).
//   "width / 2 + 5"   "2 * pi * r"   "max(t, 1.5)"
#include "Phisiks/Cad/CadApi.h"

#include <functional>
#include <string>
#include <vector>

namespace Phisiks::Cad {
    using ParameterLookup = std::function<bool(const std::string& name, double& value)>;
    // false (and `error`) for a syntax error, an unknown name, division by zero or a non-finite result.
    PHISIKS_CAD_API bool Evaluate(const std::string& expression, const ParameterLookup& lookup, double& out, std::string* error = nullptr);
    // The parameter names an expression uses (for dependency order and cycle detection).
    PHISIKS_CAD_API std::vector<std::string> ExpressionNames(const std::string& expression);
    PHISIKS_CAD_API bool IsValidParameterName(const std::string& name);
}
