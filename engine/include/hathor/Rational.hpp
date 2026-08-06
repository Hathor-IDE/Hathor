// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_RATIONAL_HPP
#define HATHOR_RATIONAL_HPP

#include <cstdint>
#include <compare>
#include <stdexcept>
#include <cstdlib>  // std::abs for int64_t

namespace hathor {

/**
 * Exact rational number (numerator/denominator pair of 64-bit integers).
 *
 * Invariants (enforced at construction and maintained by all operations):
 *  - den > 0  (sign lives in num)
 *  - gcd(|num|, den) == 1  (always in lowest terms)
 *
 * Arithmetic: +, -, *, /  all return a reduced Rational.
 * Division by zero Rational throws std::domain_error.
 * Zero denominator at construction throws std::domain_error.
 *
 * Requirement references: 2.1, 2.2, 2.3, 2.4, 2.5
 */
struct Rational {
    int64_t num;  ///< numerator; carries the sign
    int64_t den;  ///< denominator; always > 0

    // -----------------------------------------------------------------------
    // Constructors
    // -----------------------------------------------------------------------

    /// Construct from numerator and denominator.
    /// Reduces immediately. Throws std::domain_error if den == 0.
    constexpr Rational(int64_t n, int64_t d)
        : num{n}, den{d}
    {
        if (den == 0)
            throw std::domain_error("Rational: denominator cannot be zero");
        reduce(num, den);
    }

    /// Construct an integer rational (den = 1).
    constexpr explicit Rational(int64_t n)
        : num{n}, den{1}
    {}

    // -----------------------------------------------------------------------
    // Arithmetic operators
    // -----------------------------------------------------------------------

    friend Rational operator+(Rational a, Rational b);
    friend Rational operator-(Rational a, Rational b);
    friend Rational operator*(Rational a, Rational b);
    /// Throws std::domain_error if b == 0 (i.e. b.num == 0).
    friend Rational operator/(Rational a, Rational b);

    // -----------------------------------------------------------------------
    // Comparison operators
    // -----------------------------------------------------------------------

    friend bool operator==(Rational a, Rational b) noexcept
    {
        // Both are in reduced form with den > 0, so simple field equality.
        return a.num == b.num && a.den == b.den;
    }

    friend std::strong_ordering operator<=>(Rational a, Rational b) noexcept
    {
        // Compare a.num/a.den vs b.num/b.den by cross-multiplying.
        // Use __int128 on clang/GCC to avoid overflow; MSVC fallback below.
#if defined(__GNUC__) || defined(__clang__)
        __int128 lhs = static_cast<__int128>(a.num) * static_cast<__int128>(b.den);
        __int128 rhs = static_cast<__int128>(b.num) * static_cast<__int128>(a.den);
        if (lhs < rhs) return std::strong_ordering::less;
        if (lhs > rhs) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
#else
        // MSVC: reduce cross-products before comparing to limit overflow risk.
        // Pre-divide each pair by their cross GCD.
        int64_t g1 = gcd64(a.den < 0 ? -a.den : a.den,
                           b.den < 0 ? -b.den : b.den);
        int64_t aDenR = a.den / g1;
        int64_t bDenR = b.den / g1;
        int64_t lhsM  = a.num * bDenR;
        int64_t rhsM  = b.num * aDenR;
        if (lhsM < rhsM) return std::strong_ordering::less;
        if (lhsM > rhsM) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
#endif
    }

    // -----------------------------------------------------------------------
    // Conversion (for use ONLY at audio-callback boundaries — Req 2.4, 2.5)
    // -----------------------------------------------------------------------

    double toDouble() const noexcept
    {
        return static_cast<double>(num) / static_cast<double>(den);
    }

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Reduce num/den to lowest terms and ensure den > 0.
    static constexpr void reduce(int64_t& n, int64_t& d) noexcept
    {
        // Canonicalise sign: den is always positive.
        if (d < 0) { n = -n; d = -d; }

        // Euclidean GCD on absolute values.
        int64_t a = (n < 0) ? -n : n;
        int64_t b = d;
        while (b) {
            int64_t t = b;
            b = a % b;
            a = t;
        }
        // a == gcd(|n|, d)
        if (a > 1) { n /= a; d /= a; }
    }

    /// Standalone GCD helper (used in MSVC comparison path).
    static constexpr int64_t gcd64(int64_t a, int64_t b) noexcept
    {
        while (b) { int64_t t = b; b = a % b; a = t; }
        return a;
    }
};

// ---------------------------------------------------------------------------
// Arithmetic — defined in Rational.cpp
// ---------------------------------------------------------------------------

inline Rational operator+(Rational a, Rational b)
{
#if defined(__GNUC__) || defined(__clang__)
    // Use __int128 to avoid overflow during cross-multiplication.
    __int128 num = static_cast<__int128>(a.num) * static_cast<__int128>(b.den)
                 + static_cast<__int128>(b.num) * static_cast<__int128>(a.den);
    __int128 den = static_cast<__int128>(a.den) * static_cast<__int128>(b.den);
    // Euclidean GCD on __int128 absolute values.
    __int128 aa = num < 0 ? -num : num;
    __int128 bb = den < 0 ? -den : den;
    while (bb) { __int128 t = bb; bb = aa % bb; aa = t; }
    __int128 g = aa;
    if (g > 1) { num /= g; den /= g; }
    if (den < 0) { num = -num; den = -den; }
    return Rational{static_cast<int64_t>(num), static_cast<int64_t>(den)};
#else
    // MSVC: pre-reduce to limit overflow.
    // Reduce a.den and b.den by their GCD first.
    int64_t g = Rational::gcd64(a.den, b.den);
    int64_t aDenR = a.den / g;
    int64_t bDenR = b.den / g;
    int64_t newNum = a.num * bDenR + b.num * aDenR;
    int64_t newDen = aDenR * b.den;
    Rational::reduce(newNum, newDen);
    return Rational{newNum, newDen};
#endif
}

inline Rational operator-(Rational a, Rational b)
{
    return a + Rational{-b.num, b.den};
}

inline Rational operator*(Rational a, Rational b)
{
    // Cross-reduce before multiplying to keep values small.
    int64_t g1 = Rational::gcd64((a.num < 0 ? -a.num : a.num), b.den);
    int64_t g2 = Rational::gcd64((b.num < 0 ? -b.num : b.num), a.den);
    int64_t newNum = (a.num / g1) * (b.num / g2);
    int64_t newDen = (a.den / g2) * (b.den / g1);
    Rational::reduce(newNum, newDen);
    return Rational{newNum, newDen};
}

inline Rational operator/(Rational a, Rational b)
{
    if (b.num == 0)
        throw std::domain_error("Rational: division by zero");
    // Invert b, then multiply.
    // b.den is always > 0, b.num may be negative — handle sign.
    int64_t invNum = b.den;
    int64_t invDen = b.num;  // may be negative; reduce() will fix sign
    Rational inv{invNum, invDen};  // construction calls reduce which fixes den>0
    return a * inv;
}

} // namespace hathor

#endif // HATHOR_RATIONAL_HPP
