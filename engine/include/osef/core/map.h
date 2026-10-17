#pragma once
// OSEF core: Pair<A, B> and Map<K, V>, an ordered map stored as a sorted
// Vector<Pair<K, V>>.
//
//   * Keys are ordered with operator<; iteration visits keys in order.
//   * Iterators are Pair<K, V>* and are invalidated by any insertion or
//     erase (elements shift). Erase while iterating with the returned
//     iterator: `it = m.erase(it);`.
//   * Lookup is O(log n); insert/erase are O(n) (memory-compact, cache
//     friendly, ideal for the small/medium tables an OS kernel keeps).
//   * find/contains/count/erase/lower_bound accept any key type comparable
//     with K (e.g. Map<String, V>::find("name") or find(StringView)).

#include <osef/core/base.h>
#include <osef/core/vector.h>

namespace osef {

template <class A, class B>
struct Pair {
    A first;
    B second;
};

template <class A, class B>
bool operator==(const Pair<A, B>& x, const Pair<A, B>& y) {
    return x.first == y.first && x.second == y.second;
}
template <class A, class B>
bool operator!=(const Pair<A, B>& x, const Pair<A, B>& y) {
    return !(x == y);
}

template <class K, class V>
class Map {
public:
    using key_type = K;
    using mapped_type = V;
    using value_type = Pair<K, V>;
    using iterator = Pair<K, V>*;
    using const_iterator = const Pair<K, V>*;

    Map() = default;
    // Duplicate keys keep the first value.
    Map(std::initializer_list<Pair<K, V>> list) {
        for (const Pair<K, V>& p : list) emplace(p.first, p.second);
    }

    // --- lookup -----------------------------------------------------------
    template <class K2>
    iterator find(const K2& key) {
        const size_t i = lower_index(key);
        return i < items_.size() && !(key < items_.data()[i].first) ? items_.begin() + i : items_.end();
    }
    template <class K2>
    const_iterator find(const K2& key) const {
        const size_t i = lower_index(key);
        return i < items_.size() && !(key < items_.data()[i].first) ? items_.begin() + i : items_.end();
    }
    template <class K2>
    bool contains(const K2& key) const {
        return find(key) != end();
    }
    template <class K2>
    size_t count(const K2& key) const {
        return contains(key) ? 1 : 0;
    }
    // First element whose key is not less than `key`.
    template <class K2>
    iterator lower_bound(const K2& key) {
        return items_.begin() + lower_index(key);
    }
    template <class K2>
    const_iterator lower_bound(const K2& key) const {
        return items_.begin() + lower_index(key);
    }
    // First element whose key is greater than `key`.
    template <class K2>
    iterator upper_bound(const K2& key) {
        return items_.begin() + upper_index(key);
    }
    template <class K2>
    const_iterator upper_bound(const K2& key) const {
        return items_.begin() + upper_index(key);
    }

    // Asserts the key exists.
    template <class K2>
    V& at(const K2& key) {
        iterator it = find(key);
        OSEF_ASSERT(it != end());
        return it->second;
    }
    template <class K2>
    const V& at(const K2& key) const {
        const_iterator it = find(key);
        OSEF_ASSERT(it != end());
        return it->second;
    }

    // Returns the value for key, inserting a value-initialized one if absent.
    V& operator[](const K& key) {
        const size_t i = lower_index(key);
        if (i == items_.size() || key < items_.data()[i].first)
            items_.insert(items_.begin() + i, Pair<K, V>{key, V()});
        return items_.data()[i].second;
    }
    V& operator[](K&& key) {
        const size_t i = lower_index(key);
        if (i == items_.size() || key < items_.data()[i].first)
            items_.insert(items_.begin() + i, Pair<K, V>{osef::move(key), V()});
        return items_.data()[i].second;
    }

    // --- modifiers --------------------------------------------------------
    // Inserts (key, value) if the key is absent. Returns true if inserted;
    // an existing value is never overwritten.
    template <class KK, class VV>
    bool emplace(KK&& key, VV&& value) {
        const size_t i = lower_index(key);
        if (i < items_.size() && !(key < items_.data()[i].first)) return false;
        items_.insert(items_.begin() + i, Pair<K, V>{K(osef::forward<KK>(key)), V(osef::forward<VV>(value))});
        return true;
    }
    template <class KK, class VV>
    bool insert(KK&& key, VV&& value) {
        return emplace(osef::forward<KK>(key), osef::forward<VV>(value));
    }
    bool insert(const Pair<K, V>& item) { return emplace(item.first, item.second); }
    bool insert(Pair<K, V>&& item) { return emplace(osef::move(item.first), osef::move(item.second)); }

    // Inserts or overwrites. Returns true if a new key was inserted.
    template <class KK, class VV>
    bool insert_or_assign(KK&& key, VV&& value) {
        const size_t i = lower_index(key);
        if (i < items_.size() && !(key < items_.data()[i].first)) {
            items_.data()[i].second = osef::forward<VV>(value);
            return false;
        }
        items_.insert(items_.begin() + i, Pair<K, V>{K(osef::forward<KK>(key)), V(osef::forward<VV>(value))});
        return true;
    }

    // Erases the key if present; returns the number of elements removed.
    template <class K2, enable_if_t<!is_convertible_v<const K2&, const_iterator>, int> = 0>
    size_t erase(const K2& key) {
        iterator it = find(key);
        if (it == end()) return 0;
        items_.erase(it);
        return 1;
    }
    // Erases one element; returns the iterator to the element after it.
    iterator erase(iterator pos) { return items_.erase(pos); }
    iterator erase(const_iterator pos) { return items_.erase(pos); }

    void clear() noexcept { items_.clear(); }
    void reserve(size_t n) { items_.reserve(n); }
    void swap(Map& other) noexcept { items_.swap(other.items_); }

    // --- iteration --------------------------------------------------------
    iterator begin() noexcept { return items_.begin(); }
    iterator end() noexcept { return items_.end(); }
    const_iterator begin() const noexcept { return items_.begin(); }
    const_iterator end() const noexcept { return items_.end(); }
    size_t size() const noexcept { return items_.size(); }
    bool empty() const noexcept { return items_.empty(); }

private:
    template <class K2>
    size_t lower_index(const K2& key) const {
        size_t lo = 0;
        size_t hi = items_.size();
        const Pair<K, V>* d = items_.data();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (d[mid].first < key) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    template <class K2>
    size_t upper_index(const K2& key) const {
        size_t lo = 0;
        size_t hi = items_.size();
        const Pair<K, V>* d = items_.data();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (key < d[mid].first) hi = mid;
            else lo = mid + 1;
        }
        return lo;
    }

    Vector<Pair<K, V>> items_;
};

} // namespace osef
