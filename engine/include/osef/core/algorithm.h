#pragma once
// OSEF core: small algorithm set over iterator ranges.
//
// sort() is stable: insertion sort for short ranges, merge sort (O(n log n),
// with a temporary buffer) for longer ones. It needs random-access
// iterators (pointers, Vector/Map iterators). Call these qualified
// (osef::sort) when the element type lives in namespace std.

#include <osef/core/base.h>
#include <osef/core/vector.h>

namespace osef {

namespace detail {

template <class It, class Cmp>
void insertion_sort(It first, It last, Cmp& cmp) {
    if (first == last) return;
    for (It i = first + 1; i != last; ++i) {
        auto tmp = osef::move(*i);
        It j = i;
        while (j != first && cmp(tmp, *(j - 1))) {
            *j = osef::move(*(j - 1));
            --j;
        }
        *j = osef::move(tmp);
    }
}

template <class It, class Cmp, class Buf>
void merge_sort(It first, It last, Cmp& cmp, Buf& buf) {
    const auto n = last - first;
    if (n <= 16) {
        insertion_sort(first, last, cmp);
        return;
    }
    It mid = first + n / 2;
    merge_sort(first, mid, cmp, buf);
    merge_sort(mid, last, cmp, buf);
    if (!cmp(*mid, *(mid - 1))) return; // already ordered
    // Move the left half out, then merge back; taking from the left run on
    // ties keeps the sort stable.
    buf.clear();
    for (It i = first; i != mid; ++i) buf.emplace_back(osef::move(*i));
    size_t l = 0;
    const size_t ln = buf.size();
    It r = mid;
    It out = first;
    while (l < ln && r != last) {
        if (cmp(*r, buf[l])) {
            *out = osef::move(*r);
            ++r;
        } else {
            *out = osef::move(buf[l]);
            ++l;
        }
        ++out;
    }
    while (l < ln) {
        *out = osef::move(buf[l]);
        ++l;
        ++out;
    }
}

struct Less {
    template <class A, class B>
    bool operator()(const A& a, const B& b) const {
        return a < b;
    }
};

} // namespace detail

template <class It, class Cmp>
void sort(It first, It last, Cmp cmp) {
    if (last - first <= 16) {
        detail::insertion_sort(first, last, cmp);
        return;
    }
    Vector<remove_cvref_t<decltype(*first)>> buf;
    buf.reserve(static_cast<size_t>((last - first) / 2 + 1));
    detail::merge_sort(first, last, cmp, buf);
}
template <class It>
void sort(It first, It last) {
    osef::sort(first, last, detail::Less{});
}

template <class It, class Pred>
It find_if(It first, It last, Pred pred) {
    for (; first != last; ++first)
        if (pred(*first)) return first;
    return last;
}
template <class It, class T>
It find(It first, It last, const T& value) {
    for (; first != last; ++first)
        if (*first == value) return first;
    return last;
}
template <class It, class Pred>
bool any_of(It first, It last, Pred pred) {
    return osef::find_if(first, last, pred) != last;
}
template <class It, class Pred>
bool all_of(It first, It last, Pred pred) {
    for (; first != last; ++first)
        if (!pred(*first)) return false;
    return true;
}
template <class It, class Pred>
bool none_of(It first, It last, Pred pred) {
    return !osef::any_of(first, last, pred);
}
template <class It, class Pred>
size_t count_if(It first, It last, Pred pred) {
    size_t n = 0;
    for (; first != last; ++first)
        if (pred(*first)) ++n;
    return n;
}
// Reverses a bidirectional range in place.
template <class It>
void reverse(It first, It last) {
    while (first != last) {
        --last;
        if (first == last) break;
        osef::swap(*first, *last);
        ++first;
    }
}

} // namespace osef
