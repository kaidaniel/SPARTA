/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <ostream>

#include <boost/container/flat_map.hpp>

#include <sparta/PatriciaTreeCore.h>

namespace sparta {

/*
 * Represents a map implemented with a sorted vector.
 *
 * It is similar to `boost::container::flat_map` but provides map operations
 * such as union and intersection, using the same interface as
 * `PatriciaTreeMap`.
 */
template <typename Key,
          typename ValueType,
          typename Value = pt_core::SimpleValue<ValueType>,
          typename KeyCompare = std::less<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename AllocatorOrContainer =
              boost::container::new_allocator<std::pair<Key, ValueType>>>
class FlatMap final {
 private:
  using BoostFlatMap = boost::container::
      flat_map<Key, ValueType, KeyCompare, AllocatorOrContainer>;

 public:
  // C++ container concept member types
  using key_type = Key;
  using mapped_type = typename Value::type;
  using value_type = typename BoostFlatMap::value_type;
  using iterator = typename BoostFlatMap::const_iterator;
  using const_iterator = iterator;
  using difference_type = typename BoostFlatMap::difference_type;
  using size_type = typename BoostFlatMap::size_type;
  using const_reference = typename BoostFlatMap::const_reference;
  using const_pointer = typename BoostFlatMap::const_pointer;

  static_assert(std::is_same_v<ValueType, mapped_type>,
                "ValueType must be equal to Value::type");

 private:
  struct ComparePairWithKey {
    bool operator()(const value_type& pair, const Key& key) const {
      return KeyCompare()(pair.first, key);
    }
  };

  struct PairEqual {
    bool operator()(const value_type& left, const value_type& right) const {
      return KeyEqual()(left.first, right.first) &&
             Value::equals(left.second, right.second);
    }
  };

  void erase_default_values() {
    this->filter([](const Key&, const mapped_type& value) {
      return !Value::is_default_value(value);
    });
  }

 public:
  explicit FlatMap() = default;

  explicit FlatMap(std::initializer_list<std::pair<Key, ValueType>> l) {
    for (const auto& p : l) {
      insert_or_assign(p.first, p.second);
    }
  }

  bool empty() const { return m_map.empty(); }

  size_t size() const { return m_map.size(); }

  size_t max_size() const { return m_map.max_size(); }

  iterator begin() const { return m_map.cbegin(); }

  iterator end() const { return m_map.cend(); }

  const mapped_type& at(const Key& key) const {
    auto it = m_map.find(key);
    if (it == m_map.end()) {
      static const ValueType default_value = Value::default_value();
      return default_value;
    } else {
      return it->second;
    }
  }

  FlatMap& remove(const Key& key) {
    m_map.erase(key);
    return *this;
  }

  template <typename V>
  FlatMap& insert_or_assign(const Key& key, V&& value) {
    if (Value::is_default_value(value)) {
      remove(key);
    } else {
      m_map.insert_or_assign(key, std::forward<V>(value));
    }
    return *this;
  }

  template <typename Operation> // void(mapped_type*)
  FlatMap& update(Operation&& operation, const Key& key) {
    auto [it, inserted] = m_map.try_emplace(key, Value::default_value());
    operation(&it->second);
    if (Value::is_default_value(it->second)) {
      m_map.erase(it);
    }
    return *this;
  }

 private:
  bool leq_when_default_is_top(const FlatMap& other) const {
    if (m_map.size() < other.m_map.size()) {
      // In this case, there is a key bound to a non-Top value in 'other'
      // that is not defined in 'this' (and is therefore implicitly bound to
      // Top).
      return false;
    }

    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (other_it != other_end) {
      if (std::distance(it, end) < std::distance(other_it, other_end)) {
        // Same logic as above: there is a key bound to a non-Top value between
        // [other_it, other_end] that is not defined within [it, end].
        return false;
      }
      // Performs a binary search (in O(log(n))) which returns an iterator on
      // the first pair where `it->first >= other_it->first`.
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey());
      if (it == end || !KeyEqual()(it->first, other_it->first)) {
        return false;
      }
      if (!Value::leq(it->second, other_it->second)) {
        return false;
      } else {
        ++it;
        ++other_it;
      }
    }
    return true;
  }

  bool leq_when_default_is_bottom(const FlatMap& other) const {
    if (m_map.size() > other.m_map.size()) {
      // `this` has at least one non-default binding that `other` doesn't have.
      // There exists a key such that this[key] != Bottom and other[key] ==
      // Bottom.
      return false;
    }

    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (it != end) {
      if (std::distance(it, end) > std::distance(other_it, other_end)) {
        // Same logic as above: there is a non-default binding in [it, end]
        // that does not exist in [it, end], hence is bound to bottom.
        return false;
      }
      // Performs a binary search (in O(log(n))) which returs an iterator on the
      // first pair where `other_it->first >= it->first`.
      other_it = std::lower_bound(other_it, other_end, it->first,
                                  ComparePairWithKey());
      if (other_it == other_end || !KeyEqual()(it->first, other_it->first)) {
        return false;
      }
      if (!Value::leq(it->second, other_it->second)) {
        return false;
      } else {
        ++it;
        ++other_it;
      }
    }

    return true;
  }

 public:
  bool leq(const FlatMap& other) const {
    static_assert(std::is_base_of_v<AbstractDomain<ValueType>, ValueType>,
                  "leq can only be used when Value implements AbstractDomain");

    // Assumes Value::default_value() is either Top or Bottom.
    if (Value::default_value().is_top()) {
      return this->leq_when_default_is_top(other);
    } else if (Value::default_value().is_bottom()) {
      return this->leq_when_default_is_bottom(other);
    } else {
      RUNTIME_CHECK(false, undefined_operation());
    }
  }

  bool equals(const FlatMap& other) const {
    return std::equal(m_map.begin(), m_map.end(), other.m_map.begin(),
                      other.m_map.end(), PairEqual());
  }

  friend bool operator==(const FlatMap& m1, const FlatMap& m2) {
    return m1.equals(m2);
  }

  friend bool operator!=(const FlatMap& m1, const FlatMap& m2) {
    return !m1.equals(m2);
  }

  template <typename MappingFunction> // void(mapped_type*)
  void map(MappingFunction&& f) {
    bool has_default_value = false;
    for (auto& p : m_map) {
      f(&p.second);
      if (Value::is_default_value(p.second)) {
        has_default_value = true;
      }
    }
    if (has_default_value) {
      erase_default_values();
    }
  }

  template <typename Predicate> // bool(const Key&, const ValueType&)
  FlatMap& filter(Predicate&& predicate) {
    // Use boost `flat_map` API to get the underlying container and
    // apply a remove_if + erase. This allows to perform a filter in O(n).
    auto container = m_map.extract_sequence();
    container.erase(std::remove_if(container.begin(),
                                   container.end(),
                                   [predicate = std::forward<Predicate>(
                                        predicate)](const auto& p) {
                                     return !predicate(p.first, p.second);
                                   }),
                    container.end());
    m_map.adopt_sequence(boost::container::ordered_unique_range,
                         std::move(container));
    return *this;
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  void union_with(const CombiningFunction& combine, const FlatMap& other) {
    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (other_it != other_end) {
      it = std::lower_bound(it, end, other_it->first, ComparePairWithKey());
      if (it == end) {
        m_map.insert(boost::container::ordered_unique_range, other_it,
                     other_end);
        break;
      }
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
      } else {
        it = m_map.insert(it, *other_it);
        end = m_map.end();
      }
      ++it;
      ++other_it;
    }
    erase_default_values();
  }

  // Requires CombiningFunction to coerce to
  // std::function<void(mapped_type*, const mapped_type&)>
  template <typename CombiningFunction>
  void intersection_with(const CombiningFunction& combine,
                         const FlatMap& other) {
    auto it = m_map.begin(), end = m_map.end();
    auto other_it = other.m_map.begin(), other_end = other.m_map.end();
    while (it != end) {
      other_it = std::lower_bound(other_it, other_end, it->first,
                                  ComparePairWithKey());
      if (other_it == other_end) {
        m_map.erase(it, end);
        break;
      }
      if (KeyEqual()(it->first, other_it->first)) {
        combine(&it->second, other_it->second);
        ++it;
        ++other_it;
      } else {
        it->second = Value::default_value();
        ++it;
      }
    }
    erase_default_values();
  }

  void clear() { m_map.clear(); }

  friend std::ostream& operator<<(std::ostream& o, const FlatMap& m) {
    using namespace sparta;
    o << "{";
    for (auto it = m.begin(); it != m.end(); ++it) {
      o << pt_util::deref(it->first) << " -> " << it->second;
      if (std::next(it) != m.end()) {
        o << ", ";
      }
    }
    o << "}";
    return o;
  }

 private:
  BoostFlatMap m_map;
};

} // namespace sparta
