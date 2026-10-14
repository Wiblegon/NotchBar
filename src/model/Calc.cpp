#include "model/Calc.h"

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace nb::calc {
namespace {

bool IsOp(wchar_t c) {
    return c == L'+' || c == L'-' || c == L'*' || c == L'/' || c == L'x' || c == L'X' || c == L'×' || c == L'÷' ||
           c == L'−';
}
bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsSep(wchar_t c) { return c == L'.' || c == L','; }

enum class Tok { Num, Plus, Minus, Mul, Div, LParen, RParen, End };
struct Token {
    Tok t;
    double v = 0;
};

// Interprets a numeric literal containing digits plus '.'/',' separators.
bool ParseNumber(std::wstring_view raw, double& out) {
    int dots = 0, commas = 0;
    size_t lastDot = std::wstring_view::npos, lastComma = std::wstring_view::npos;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'.') { ++dots; lastDot = i; }
        if (raw[i] == L',') { ++commas; lastComma = i; }
    }
    wchar_t decimal = 0;
    if (dots && commas) decimal = lastDot > lastComma ? L'.' : L',';      // "1.234,5" or "1,234.5"
    else if (dots == 1) decimal = L'.';
    else if (commas == 1) decimal = L',';
    // dots>1 or commas>1 with the other absent: grouping only ("1.234.567").
    std::string norm;
    bool seenDecimal = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        wchar_t c = raw[i];
        if (IsDigit(c)) norm += static_cast<char>(c);
        else if (c == decimal) {
            if (seenDecimal) return false;
            seenDecimal = true;
            norm += '.';
        } else {
            // grouping separator: must be followed by exactly 3 digits
            size_t j = i + 1, n = 0;
            while (j < raw.size() && IsDigit(raw[j])) { ++j; ++n; }
            if (n != 3 || i == 0) return false;
        }
    }
    if (norm.empty() || norm == ".") return false;
    out = strtod(norm.c_str(), nullptr);
    return true;
}

struct Lexer {
    std::vector<Token> toks;
    bool ok = true;
    explicit Lexer(std::wstring_view s) {
        size_t i = 0;
        while (i < s.size()) {
            wchar_t c = s[i];
            if (c == L' ' || c == L'\t' || c == L' ' || c == L' ') { ++i; continue; }
            if (IsDigit(c) || (IsSep(c) && i + 1 < s.size() && IsDigit(s[i + 1]))) {
                size_t b = i;
                while (i < s.size() && (IsDigit(s[i]) || IsSep(s[i]))) ++i;
                double v;
                if (!ParseNumber(s.substr(b, i - b), v)) { ok = false; return; }
                toks.push_back({Tok::Num, v});
                continue;
            }
            switch (c) {
            case L'+': toks.push_back({Tok::Plus}); break;
            case L'-': case L'−': toks.push_back({Tok::Minus}); break;
            case L'*': case L'x': case L'X': case L'×': toks.push_back({Tok::Mul}); break;
            case L'/': case L'÷': case L':': toks.push_back({Tok::Div}); break;
            case L'(': toks.push_back({Tok::LParen}); break;
            case L')': toks.push_back({Tok::RParen}); break;
            default: ok = false; return;
            }
            ++i;
        }
        toks.push_back({Tok::End});
    }
};

struct Parser {
    const std::vector<Token>& t;
    size_t p = 0;
    Status st = Status::Ok;
    int depth = 0;

    Tok peek() const { return t[p].t; }
    void setErr(Status s) {
        if (st == Status::Ok) st = s;
    }

    double expr() {
        double v = term();
        while (st == Status::Ok && (peek() == Tok::Plus || peek() == Tok::Minus)) {
            Tok op = t[p++].t;
            double r = term();
            v = op == Tok::Plus ? v + r : v - r;
        }
        return v;
    }
    double term() {
        double v = unary();
        while (st == Status::Ok && (peek() == Tok::Mul || peek() == Tok::Div)) {
            Tok op = t[p++].t;
            double r = unary();
            if (st != Status::Ok) return 0;
            if (op == Tok::Mul) v *= r;
            else {
                if (r == 0) { setErr(Status::DivideByZero); return 0; }
                v /= r;
            }
        }
        return v;
    }
    double unary() {
        if (peek() == Tok::Minus) { ++p; return -unary(); }
        if (peek() == Tok::Plus) { ++p; return unary(); }
        return primary();
    }
    double primary() {
        if (++depth > 200) { setErr(Status::Invalid); return 0; }
        Tok k = peek();
        double v = 0;
        if (k == Tok::Num) { v = t[p++].v; }
        else if (k == Tok::LParen) {
            ++p;
            if (peek() == Tok::RParen) { setErr(Status::Invalid); return 0; }
            v = expr();
            if (st != Status::Ok) return 0;
            if (peek() == Tok::End) { setErr(Status::Incomplete); return 0; }
            if (peek() != Tok::RParen) { setErr(Status::Invalid); return 0; }
            ++p;
        } else if (k == Tok::End) { setErr(Status::Incomplete); }
        else { setErr(Status::Invalid); }
        --depth;
        return v;
    }
};

} // namespace

bool LooksLikeExpression(std::wstring_view s) {
    bool digit = false, op = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (IsDigit(c)) digit = true;
        else if (c == L'(' || c == L')') op = true;
        else if (IsOp(c) || c == L':') {
            // a leading '-' alone ("-5") is a number, not an operation
            if (!(c == L'-' && i == 0)) op = true;
        } else if (IsSep(c) || c == L' ' || c == L' ') {
        } else return false;
    }
    return digit && op;
}

Result Evaluate(std::wstring_view input) {
    if (!LooksLikeExpression(input)) return {Status::NotExpression, 0};
    Lexer lx(input);
    if (!lx.ok) return {Status::Invalid, 0};
    Parser ps{lx.toks};
    double v = ps.expr();
    if (ps.st == Status::Ok && ps.peek() != Tok::End) ps.setErr(ps.peek() == Tok::RParen ? Status::Invalid : Status::Invalid);
    if (ps.st != Status::Ok) return {ps.st, 0};
    if (!std::isfinite(v)) return {Status::Overflow, 0};
    if (v == 0) v = 0; // normalize -0
    return {Status::Ok, v};
}

std::wstring Format(double value, wchar_t decimalSeparator) {
    wchar_t buf[64];
    double mag = std::fabs(value);
    if (mag != 0 && (mag >= 1e15 || mag < 1e-9)) {
        swprintf(buf, 64, L"%.10g", value);
    } else {
        swprintf(buf, 64, L"%.10f", value);
        std::wstring s = buf;
        if (s.find(L'.') != std::wstring::npos) {
            while (!s.empty() && s.back() == L'0') s.pop_back();
            if (!s.empty() && s.back() == L'.') s.pop_back();
        }
        if (s == L"-0") s = L"0";
        for (auto& c : s)
            if (c == L'.') c = decimalSeparator;
        return s;
    }
    std::wstring s = buf;
    for (auto& c : s)
        if (c == L'.') c = decimalSeparator;
    return s;
}

const wchar_t* StatusMessage(Status s) {
    switch (s) {
    case Status::Ok: return L"";
    case Status::Incomplete: return L"İfade tamamlanmadı";
    case Status::DivideByZero: return L"Sıfıra bölme tanımsız";
    case Status::Overflow: return L"Sonuç hesaplanamayacak kadar büyük";
    case Status::Invalid: return L"Geçersiz ifade";
    default: return L"";
    }
}

} // namespace nb::calc
