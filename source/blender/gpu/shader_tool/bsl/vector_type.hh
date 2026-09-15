/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include <array>

namespace bsl {

/**
 * Vector type that allows all conversions (even if ill formed).
 * This is to allow the visitor pattern for constexpr evaluation.
 * The actual ill formed conversions and operators are reported as error by the compiler.
 */
template<typename T, int size = 1> struct Vec {
  using Base = T;
  std::array<T, size> values;

  Vec() = default;

  explicit Vec(T s)
  {
    for (int v = 0; v < size; ++v) {
      values[v] = s;
    }
  }

  explicit Vec(T x, T y)
    requires(size == 2)
  {
    values[0] = x;
    values[1] = y;
  }
  explicit Vec(T x, T y, T z)
    requires(size == 3)
  {
    values[0] = x;
    values[1] = y;
    values[2] = z;
  }
  explicit Vec(T x, T y, T z, T w)
    requires(size == 4)
  {
    values[0] = x;
    values[1] = y;
    values[2] = z;
    values[3] = w;
  }

  template<typename U>
  explicit Vec(const Vec<U, 1> &other)
    requires(size != 1)
  {
    for (int v = 0; v < size; ++v) {
      values[v] = T(other.values[0]);
    }
  }

  template<typename U> explicit Vec(const Vec<U, size> &other)
  {
    for (int v = 0; v < size; ++v) {
      values[v] = T(other.values[v]);
    }
  }

  template<typename U, int other_size>
  explicit Vec(const Vec<U, other_size> & /*other*/)
    requires(other_size != 1 && other_size != size)
  {
    /* ill-formed. */
    *this = Vec(T(0));
  }

  explicit operator T() const
    requires(size == 1)
  {
    return values[0];
  }

  /* --- Element Access --- */

  T &operator[](size_t i)
  {
    return values[i];
  }
  const T &operator[](size_t i) const
  {
    return values[i];
  }

  /* --- Unary Arithmetic & Bitwise Operators --- */

  Vec operator+() const
  {
    return *this;
  }

  Vec operator-() const
  {
    Vec res;
    for (int i = 0; i < size; ++i) {
      res[i] = -values[i];
    }
    return res;
  }

  Vec operator~() const
  {
    Vec res;
    for (int i = 0; i < size; ++i) {
      res[i] = ~values[i];
    }
    return res;
  }

  Vec<bool, size> operator!() const
  {
    Vec<bool, size> res;
    for (int i = 0; i < size; ++i) {
      res[i] = !values[i];
    }
    return res;
  }

  /* --- Pre/Post Increment & Decrement --- */

  Vec &operator++()
  {
    for (auto &v : values) {
      ++v;
    }
    return *this;
  }
  Vec operator++(int)
  {
    Vec tmp = *this;
    ++(*this);
    return tmp;
  }
  Vec &operator--()
  {
    for (auto &v : values) {
      --v;
    }
    return *this;
  }
  Vec operator--(int)
  {
    Vec tmp = *this;
    --(*this);
    return tmp;
  }

  /* --- Compound Assignment Operators (Vector) --- */

  Vec &operator+=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] += rhs[i];
    }
    return *this;
  }
  Vec &operator-=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] -= rhs[i];
    }
    return *this;
  }
  Vec &operator*=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] *= rhs[i];
    }
    return *this;
  }
  Vec &operator/=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] /= rhs[i];
    }
    return *this;
  }
  Vec &operator%=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] %= rhs[i];
    }
    return *this;
  }
  Vec &operator&=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] &= rhs[i];
    }
    return *this;
  }
  Vec &operator|=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] |= rhs[i];
    }
    return *this;
  }
  Vec &operator^=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] ^= rhs[i];
    }
    return *this;
  }
  Vec &operator<<=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] <<= rhs[i];
    }
    return *this;
  }
  Vec &operator>>=(const Vec &rhs)
  {
    for (int i = 0; i < size; ++i) {
      values[i] >>= rhs[i];
    }
    return *this;
  }

  /* --- Compound Assignment Operators (Scalar) --- */

  Vec &operator+=(T s)
  {
    for (auto &v : values) {
      v += s;
    }
    return *this;
  }
  Vec &operator-=(T s)
  {
    for (auto &v : values) {
      v -= s;
    }
    return *this;
  }
  Vec &operator*=(T s)
  {
    for (auto &v : values) {
      v *= s;
    }
    return *this;
  }
  Vec &operator/=(T s)
  {
    for (auto &v : values) {
      v /= s;
    }
    return *this;
  }
  Vec &operator%=(T s)
  {
    for (auto &v : values) {
      v %= s;
    }
    return *this;
  }
  Vec &operator&=(T s)
  {
    for (auto &v : values) {
      v &= s;
    }
    return *this;
  }
  Vec &operator|=(T s)
  {
    for (auto &v : values) {
      v |= s;
    }
    return *this;
  }
  Vec &operator^=(T s)
  {
    for (auto &v : values) {
      v ^= s;
    }
    return *this;
  }
  Vec &operator<<=(T s)
  {
    for (auto &v : values) {
      v <<= s;
    }
    return *this;
  }
  Vec &operator>>=(T s)
  {
    for (auto &v : values) {
      v >>= s;
    }
    return *this;
  }
};

#define BINARY_OP(op) \
  template<typename T, int N, typename U, int M> \
  Vec<decltype(T() op U()), std ::max(N, M)> operator op(const Vec<T, N> &lhs, \
                                                         const Vec<U, M> &rhs) \
  { \
    Vec<decltype(T() op U()), std ::max(N, M)> r; \
    for (int i = 0; i < std::max(N, M); ++i) { \
      r[i] = lhs[std::min(N - 1, i)] op rhs[std::min(M - 1, i)]; \
    } \
    return r; \
  }

BINARY_OP(+)
BINARY_OP(-)
BINARY_OP(*)
BINARY_OP(/)
BINARY_OP(%)
BINARY_OP(&)
BINARY_OP(|)
BINARY_OP(^)
BINARY_OP(<<)
BINARY_OP(>>)
BINARY_OP(&&)
BINARY_OP(||)
BINARY_OP(==)
BINARY_OP(!=)
BINARY_OP(<)
BINARY_OP(<=)
BINARY_OP(>)
BINARY_OP(>=)

#undef BINARY_OP

template<typename T, int N> bool contains_zero(const Vec<T, N> &v)
{
  for (int i = 0; i < N; ++i) {
    if (v[i] == 0) {
      return true;
    }
  }
  return false;
}

template<typename T, int N> bool anyLessThan(const Vec<T, N> &lhs, int rhs)
{
  for (int i = 0; i < N; ++i) {
    if (lhs[i] < rhs) {
      return true;
    }
  }
  return false;
}

template<typename T, int N> bool anyGreaterThanEqual(const Vec<T, N> &lhs, int rhs)
{
  for (int i = 0; i < N; ++i) {
    if (lhs[i] >= rhs) {
      return true;
    }
  }
  return false;
}

}  // namespace bsl
