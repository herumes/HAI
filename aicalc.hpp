#pragma once
// Small, safe helpers behind the AI's calculator and dice tools.
#include <functional>
#include <string>

namespace AiCalc {

// + - * / % ^ ( ), unary minus, and sqrt abs floor ceil round ln log sin cos tan min max,
// plus the constants pi and e. No variables, no I/O. On failure returns false with `err` set.
bool Eval(const std::string& expr, double& out, std::string& err);

// 12 -> "12", 0.5 -> "0.5", 1e21 -> "1e+21": up to 10 significant digits, no trailing zeros.
std::string FormatNumber(double v);

// "d20", "2d6+3", "4d8-1": at most 100 dice of at most 1000 sides. `roll(sides)` returns 1..sides.
// Returns a short description ("2d6+3: 4 + 5 + 3 = 12") or an error text starting with "error".
std::string RollDice(const std::string& spec, const std::function<unsigned(unsigned sides)>& roll);

} // namespace AiCalc
