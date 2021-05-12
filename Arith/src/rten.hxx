#ifndef RTEN_HXX
#define RTEN_HXX

#include "defs.hxx"
#include "simd.hxx"
#include "vect.hxx"

#include "vec.hxx" // for dnup_t

#include <fixmath.hxx> // include this before <cctk.h>
#include <cctk.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <utility>
#include <vector>

namespace Arith {
using namespace std;

// Riemann tensor
template <typename T, int D, dnup_t dnup1, dnup_t dnup2, dnup_t dnup3,
          dnup_t dnup4>
struct rten {

  // template <typename, int, dnup_t, dnup_t, dnup_t, dnup_t> friend class rten;

  // We omit the first Bianchi identity
  //   R_abcd = - R_abdc
  //   R_abcd = - R_bacd
  //   R_abcd = + R_cdab
  constexpr static int N0 =
      D * (D - 1) / 2; // antisymmetric within each index pair
  constexpr static int N = N0 * (N0 + 1) / 2; // symmetric index pairs
  vect<T, N> elts;

public:
  typedef T value_type;
  typedef int size_type;
  static constexpr int size_value = N;

private:
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST int
  ind(const int i, const int j, const int k, const int l) {
    using std::max, std::min;
#ifdef CCTK_DEBUG
    assert(i >= 0 && i < D);
    assert(j >= 0 && j < D);
    assert(k >= 0 && k < D);
    assert(l >= 0 && l < D);
#endif
    if (i == j || k == l)
      return 0;
    const int i1 = min(i, j);
    const int j1 = max(i, j);
    const int k1 = min(k, l);
    const int l1 = max(k, l);
#ifdef CCTK_DEBUG
    assert(i1 >= 0 && i1 < j1 && j1 < D);
    assert(k1 >= 0 && k1 < l1 && l1 < D);
#endif
    const int ij = i1 * (2 * D - 3 - i1) / 2 + j1 - 1; // antisymmetric
    const int kl = k1 * (2 * D - 3 - k1) / 2 + l1 - 1; // antisymmetric
#ifdef CCTK_DEBUG
    assert(ij >= 0 && ij < N0);
    assert(kl >= 0 && kl < N0);
#endif
    const int ij1 = min(ij, kl);
    const int kl1 = max(ij, kl);
#ifdef CCTK_DEBUG
    assert(ij1 >= 0 && ij1 <= kl1 && kl1 < N0);
#endif
    const int n = ij1 * (2 * N0 - 1 - ij1) / 2 + kl1; // symmetric
#ifdef CCTK_DEBUG
    assert(n >= 0 && n < N);
#endif
    return n;
  }
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST int
  sign(const int i, const int j, const int k, const int l) {
    if (i == j || k == l)
      return 0;
    return (i < j) == (k < l) ? 1 : -1;
  }

public:
  // initializes all elements to nan
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten()
      : elts(nan<vect<T, N> >()) {}

  constexpr ARITH_INLINE rten(const rten &) = default;
  constexpr ARITH_INLINE rten(rten &&) = default;
  constexpr ARITH_INLINE rten &operator=(const rten &) = default;
  constexpr ARITH_INLINE rten &operator=(rten &&) = default;

  template <typename U>
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
  rten(rten<U, D, dnup1, dnup2, dnup3, dnup4> x)
      : elts(move(x.elts)) {}

  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten(initializer_list<T> x)
      : elts(x) {}
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten(vect<T, N> elts)
      : elts(move(elts)) {}
  explicit constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten(array<T, N> x)
      : elts(move(x)) {}
  // explicit constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten(const
  // vector<T> &x) : elts(x) {} explicit constexpr ARITH_INLINE ARITH_DEVICE
  // ARITH_HOST rten(vector<T> &&x) : elts(move(x)) {}

  explicit constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
  operator vect<T, N>() const {
    return elts;
  }

  template <typename F>
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST void loop(const F &f) {
    for (int i = 0; i < D; ++i)
      for (int j = i + 1; j < D; ++j)
        for (int k = 0; k < D; ++k)
          for (int l = k + 1; l < D; ++l)
            if (D * i + j <= D * k + l)
              f(i, j, k, l);
  }
  template <typename F, typename = result_of_t<F(int, int, int, int)> >
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten(const F &f)
      : rten(fmap(f, iota1(), iota2(), iota3(), iota4())) {}

  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten pure(const T &a) {
    return {vect<T, N>::pure(a)};
  }
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten unit(int i, int j,
                                                                  int k,
                                                                  int l) {
    rten r = zero<rten>();
    r(i, j, k, l) = one<T>() / sign(i, j, k, l);
    return r;
  }

  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<int, D, dnup1, dnup2, dnup3, dnup4>
      iota() {
    rten<int, D, dnup1, dnup2, dnup3, dnup4> r;
    rten<int, D, dnup1, dnup2, dnup3, dnup4>::loop(
        [&](int i, int j, int k, int l) {
          r(i, j, k, l) = {i, j, k, l};
        });
    return r;
  }
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<int, D, dnup1, dnup2, dnup3, dnup4>
      iota1() {
    rten<int, D, dnup1, dnup2, dnup3, dnup4> r;
    rten<int, D, dnup1, dnup2, dnup3, dnup4>::loop(
        [&](int i, int j, int k, int l) { r.set(i, j, k, l, i); });
    return r;
  }
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<int, D, dnup1, dnup2, dnup3, dnup4>
      iota2() {
    rten<int, D, dnup1, dnup2, dnup3, dnup4> r;
    rten<int, D, dnup1, dnup2, dnup3, dnup4>::loop(
        [&](int i, int j, int k, int l) { r.set(i, j, k, l, j); });
    return r;
  }
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<int, D, dnup1, dnup2, dnup3, dnup4>
      iota3() {
    rten<int, D, dnup1, dnup2, dnup3, dnup4> r;
    rten<int, D, dnup1, dnup2, dnup3, dnup4>::loop(
        [&](int i, int j, int k, int l) { r.set(i, j, k, l, k); });
    return r;
  }
  static constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<int, D, dnup1, dnup2, dnup3, dnup4>
      iota4() {
    rten<int, D, dnup1, dnup2, dnup3, dnup4> r;
    rten<int, D, dnup1, dnup2, dnup3, dnup4>::loop(
        [&](int i, int j, int k, int l) { r.set(i, j, k, l, l); });
    return r;
  }

  template <typename F, typename... Args,
            typename R =
                remove_cv_t<remove_reference_t<result_of_t<F(T, Args...)> > > >
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<R, D, dnup1, dnup2, dnup3, dnup4>
      fmap(const F &f, const rten &x,
           const rten<Args, D, dnup1, dnup2, dnup3, dnup4> &...args) {
    return rten<R, D, dnup1, dnup2, dnup3, dnup4>(
        fmap(f, x.elts, args.elts...));
  }
  template <typename F, typename... Args>
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST void
  fmap_(const F &f, const rten &x,
        const rten<Args, D, dnup1, dnup2, dnup3, dnup4> &...args) {
    fmap_(f, x.args, args.elts...);
  }

  template <
      typename... Args,
      typename R = remove_cv_t<remove_reference_t<result_of_t<T(Args...)> > > >
  ARITH_INLINE ARITH_DEVICE ARITH_HOST rten<R, D, dnup1, dnup2, dnup3, dnup4>
  operator()(const Args &...args) const {
    return fmap([&](const T &var) { return var(args...); }, *this);
  }
  template <typename Arg1, typename Arg2, typename U,
            typename R = remove_cv_t<
                remove_reference_t<result_of_t<T(Arg1, Arg2, U)> > > >
  ARITH_INLINE ARITH_DEVICE ARITH_HOST rten<R, D, dnup1, dnup2, dnup3, dnup4>
  operator()(const Arg1 &arg1, const Arg2 &arg2,
             const rten<U, D, dnup1, dnup2, dnup3, dnup4> &val) const {
    return fmap([&](const T &var, const U &x) { return var(arg1, arg2, x); },
                *this, val);
  }
  // template <typename... Args, typename U, typename T1 = T,
  //           typename = decltype(declval<T1>().store(declval<Args>()...,
  //                                                   declval<U>()))>
  // ARITH_INLINE ARITH_DEVICE ARITH_HOST void store(const Args &...args,
  //                         const rten<U,  dnup1, dnup2, dnup3,dnup4> &val)
  //                         const {
  //   fmap_([&](const auto &var, const auto &x) { return var.store(args..., x);
  //   },
  //        val);
  // }
  template <typename Arg1, typename Arg2, typename U, typename T1 = T,
            typename = decltype(declval<T1>().store(
                declval<Arg1>(), declval<Arg2>(), declval<U>()))>
  ARITH_INLINE ARITH_DEVICE ARITH_HOST void
  store(const Arg1 &arg1, const Arg2 &arg2,
        const rten<U, D, dnup1, dnup2, dnup3, dnup4> &val) const {
    fmap_([&](const T &var, const U &x) { return var.store(arg1, arg2, x); },
          *this, val);
  }

  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST size_type size() const {
    return N;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST T operator()(int i, int j,
                                                              int k,
                                                              int l) const {
    return sign(i, j, k, l) * elts[ind(i, j, k, l)];
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST void set(int i, int j, int k,
                                                          int l, const T &x) {
#ifdef CCTK_DEBUG
    assert(sign(i, j, k, l) == 1);
#endif
    elts[ind(i, j, k, l)] = x;
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator+(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x) {
    return {+x.elts};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator-(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x) {
    return {-x.elts};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator+(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x,
                const rten<T, D, dnup1, dnup2, dnup3, dnup4> &y) {
    return {x.elts + y.elts};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator-(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x,
                const rten<T, D, dnup1, dnup2, dnup3, dnup4> &y) {
    return {x.elts - y.elts};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator*(const T &a, const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x) {
    return {a * x.elts};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator*(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x, const T &a) {
    return {x.elts * a};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator/(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x, const T &a) {
    return {x.elts / a};
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST
      rten<T, D, dnup1, dnup2, dnup3, dnup4>
      operator%(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x, const T &a) {
    return {x.elts % a};
  }

  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten
  operator+=(const rten &x) {
    return *this = *this + x;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten
  operator-=(const rten &x) {
    return *this = *this - x;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten operator*=(const T &a) {
    return *this = *this * a;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten operator/=(const T &a) {
    return *this = *this / a;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST rten operator%=(const T &a) {
    return *this = *this % a;
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST auto /*bool*/
  operator==(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x,
             const rten<T, D, dnup1, dnup2, dnup3, dnup4> &y) {
    return all(x.elts == y.elts);
  }
  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST auto /*bool*/
  operator!=(const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x,
             const rten<T, D, dnup1, dnup2, dnup3, dnup4> &y) {
    return !(x == y);
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST auto /*bool*/
  all(const rten &x) {
    return all(x.elts);
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST auto /*bool*/
  any(const rten &x) {
    return any(x.elts);
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST auto /*bool*/
  anyisnan(const rten &x) {
    return anyisnan(x.elts);
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE
      ARITH_HOST auto /*rten<bool, D, dnup1, dnup2, dnup3, dnup4>*/
      isnan(const rten &x) {
    return isnan(x.elts);
  }

  friend constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST T
  maxabs(const rten &x) {
    return maxabs(x.elts);
  }

  friend ostream &operator<<(ostream &os,
                             const rten<T, D, dnup1, dnup2, dnup3, dnup4> &x) {
    os << "(" << dnup1 << dnup2 << dnup3 << dnup4 << ")[\n";
    rten<T, D, dnup1, dnup2, dnup3, dnup4>::loop(
        [&](int i, int j, int k, int l) {
          os << i << "," << j << "," << k << "," << l << ":" << x(i, j, k, l)
             << "\n";
        });
    os << "]";
    return os;
  }
};

template <typename T, int D, dnup_t dnup1, dnup_t dnup2, dnup_t dnup3,
          dnup_t dnup4>
struct zero<rten<T, D, dnup1, dnup2, dnup3, dnup4> > {
  typedef rten<T, D, dnup1, dnup2, dnup3, dnup4> value_type;
  static constexpr value_type value =
      rten<T, D, dnup1, dnup2, dnup3, dnup4>::pure(zero<T>::value);
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST operator value_type() const {
    return value;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST value_type operator()() const {
    return value;
  }
};

template <typename T, int D, dnup_t dnup1, dnup_t dnup2, dnup_t dnup3,
          dnup_t dnup4>
struct nan<rten<T, D, dnup1, dnup2, dnup3, dnup4> > {
  typedef rten<T, D, dnup1, dnup2, dnup3, dnup4> value_type;
  static constexpr value_type value =
      rten<T, D, dnup1, dnup2, dnup3, dnup4>::pure(nan<T>::value);
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST operator value_type() const {
    return value;
  }
  constexpr ARITH_INLINE ARITH_DEVICE ARITH_HOST value_type operator()() const {
    return value;
  }
};

////////////////////////////////////////////////////////////////////////////////

template <typename T, int D, dnup_t dnup1, dnup_t dnup2, dnup_t dnup3,
          dnup_t dnup4>
constexpr rten<simd<T>, D, dnup1, dnup2, dnup3, dnup4>
if_else(const simdl<T> &cond,
        const rten<simd<T>, D, dnup1, dnup2, dnup3, dnup4> &x,
        const rten<simd<T>, D, dnup1, dnup2, dnup3, dnup4> &y) {
  return fmap([&](const auto &x, const auto &y) { return if_else(cond, x, y); },
              x, y);
}

} // namespace Arith

#endif // #ifndef RTEN_HXX