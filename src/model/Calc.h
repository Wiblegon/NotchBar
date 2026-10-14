#pragma once
// Safe arithmetic evaluator for the launcher. Pure recursive-descent parser:
// no shell, no scripting engine, no code execution. Supports + - * / ( ), unary minus,
// decimals with '.' or ',' and grouping separators.
#include <string>
#include <string_view>

namespace nb::calc {

enum class Status {
    NotExpression, // input does not look like arithmetic (e.g. "spotify", or a bare number)
    Ok,
    Incomplete,    // user is still typing: trailing operator or unclosed parenthesis
    DivideByZero,
    Invalid,
    Overflow,
};

struct Result {
    Status status = Status::NotExpression;
    double value = 0;
};

// Cheap pre-check used on every keystroke.
bool LooksLikeExpression(std::wstring_view input);

Result Evaluate(std::wstring_view input);

// Formats with the given decimal separator; trims trailing zeros, max 10 fractional digits,
// switches to exponent notation for very large/small magnitudes.
std::wstring Format(double value, wchar_t decimalSeparator);

const wchar_t* StatusMessage(Status s); // Turkish user-facing text

} // namespace nb::calc
