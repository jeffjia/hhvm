/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   | Copyright (c) 1998-2010 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/policy_array.h"
#include "hphp/runtime/base/array_init.h"
#include "hphp/runtime/base/array_iterator.h"
#include "hphp/runtime/base/hphp_array.h"
#include "hphp/runtime/base/sort_helpers.h"
#include "folly/Foreach.h"

TRACE_SET_MOD(runtime);
#define MYLOG if (true) {} else LOG(INFO)
#define APILOG(a) MYLOG << "{" << (a) << ":m_size=" << (a)->m_size    \
  << ";cap=" << (a)->capacity() << ";m_pos=" << (a)->m_pos << "}->" \
  << __FUNCTION__

namespace HPHP {

static string keystr(const StringData* key) {
  return "s:" + string(key->data(), key->size());
}
static string keystr(int64_t key) {
  return "i:" + std::to_string(key);
}
static string valstr(const Variant& v) {
  try {
    auto result = v.toString();
    return string(result.data(), result.size());
  } catch (...) {
    return "<messedup>";
  }
}

SimpleArrayStore::SimpleArrayStore(const SimpleArrayStore& rhs,
                                   uint length, uint capacity,
                                   ArrayData::AllocationMode am,
                                   const ArrayData* owner)
    : m_capacity(std::max<uint>(startingCapacity, capacity))
    , m_nextKey(rhs.m_nextKey) {
  assert(length <= capacity && this != &rhs);
  allocate(m_keys, m_vals, m_capacity, am);
  // Copy data with flattening
  FOR_EACH_RANGE (i, 0, length) {
    tvDupFlattenVars(rhs.m_vals + i, m_vals + i, owner);
    if (rhs.hasStrKey(toPos(i))) {
      setKey(toPos(i), rhs.m_keys[i].s);
    } else {
      setKey(toPos(i), rhs.m_keys[i].i);
    }
  }
}

void SimpleArrayStore::grow(uint length, uint minCap, uint idealCap,
                            ArrayData::AllocationMode am) {
  assert(idealCap >= minCap);
  if (m_capacity >= minCap) return;
  MYLOG << (void*)this << "->grow(" << length << ", " << minCap << ", "
        << idealCap << ", " << uint(am) << "); m_capacity=" << m_capacity;
  idealCap = std::max<uint>(startingCapacity, idealCap);
  Key* newKeys;
  TypedValueAux* newVals;
  allocate(newKeys, newVals, idealCap, am);
  // Move data
  memcpy(newKeys, m_keys, length * sizeof(*m_keys));
  memcpy(newVals, m_vals, length * sizeof(*m_vals));
  deallocate(m_keys, m_vals, am);
  // Change state
  m_capacity = idealCap;
  m_keys = newKeys;
  m_vals = newVals;
}

void SimpleArrayStore::destroy(uint length, ArrayData::AllocationMode am) {
  FOR_EACH_RANGE (i, 0, length) {
    if (hasStrKey(toPos(i))) {
      auto k = m_keys[i].s;
      assert(k);
      if (!k->decRefCount()) DELETE(StringData)(k);
    }
    lval(toPos(i)).~Variant();
  }
  deallocate(m_keys, m_vals, am);
#ifndef NDEBUG
  m_keys = nullptr;
  m_vals = nullptr;
#endif
}

PosType SimpleArrayStore::find(int64_t key, uint length) const {
  assert(m_keys && length <= m_capacity);
  // glorious linear find
  for (uint i = 0; i < length; ++i) {
    if (key == m_keys[i].i && !hasStrKey(toPos(i))) {
      return toPos(i);
    }
  }
  return PosType::invalid;
}

PosType SimpleArrayStore::find(const StringData* key, uint length) const {
  // glorious linear find
  assert(key && m_keys && length <= m_capacity);
  auto const d0 = key->data();
  auto const sz = key->size();
  for (uint i = 0; i < length; ++i) {
    if (!hasStrKey(toPos(i))) continue;
    auto const k = m_keys[i].s;
    if (key == k) return toPos(i);
    assert(k);
    if (sz != k->size()) continue;
    auto const data = k->data();
    if (d0 == data) return toPos(i);
    assert(d0 && data);
    if (memcmp(d0, data, sz) == 0) return toPos(i);
  }
  return PosType::invalid;
}

template <class K>
bool SimpleArrayStore::update(K key, const Variant& val, uint length,
                              ArrayData::AllocationMode am) {
  assert(length <= m_capacity && m_vals);
  auto const pos = find(key, length);
  if (pos != PosType::invalid) {
    // found, overwrite
    assert(tvIsPlausible(m_vals + toInt<uint32_t>(pos)));
    lval(pos) = val;
    return false;
  }
  // not found, insert
  assert(length <= m_capacity);
  if (length == m_capacity) {
    grow(length, length + 1, length * 2 + 1, am);
  }
  assert(m_keys && m_vals && length < m_capacity);
  new(&lval(toPos(length))) Variant(val);
  setKey(toPos(length), key);
  return true;
}

void SimpleArrayStore::erase(PosType pos, uint length) {
  auto const ipos = toInt<uint32_t>(pos);
  assert(ipos < length && length <= capacity());
  // Destroy data at pos
  if (hasStrKey(pos)) {
    auto const k = m_keys[ipos].s;
    assert(k);
    if (!k->decRefCount()) DELETE(StringData)(k);
  }
  lval(pos).~Variant();
  // Shift over memory
  auto const itemsToMove = length - ipos - 1;
  memmove(m_keys + ipos, m_keys + ipos + 1, itemsToMove * sizeof(*m_keys));
  memmove(m_vals + ipos, m_vals + ipos + 1, itemsToMove * sizeof(*m_vals));
}

void SimpleArrayStore::prepend(const Variant& v, uint length,
                               ArrayData::AllocationMode am) {
  if (length == capacity()) {
    grow(length, length + 1, length * 2 + 1, am);
  }
  assert(length < capacity());
  // Shift stuff over
  memmove(m_keys + 1, m_keys, length * sizeof(*m_keys));
  memmove(m_vals + 1, m_vals, length * sizeof(*m_vals));
  // Construct the new value
  new(m_vals) Variant(v);
}

////////////////////////////////////////////////////////////////////////////////

IMPLEMENT_SMART_ALLOCATION(PolicyArray)

PolicyArray::PolicyArray(uint capacity)
    : ArrayData(kPolicyKind)
    , Store(m_allocMode, capacity) {
  m_size = 0;
  m_pos = invalid_index;
  // Log at the end of the ctor so as to show the properly initialized
  // members.
  APILOG(this) << "(" << capacity << ");";
}

PolicyArray::PolicyArray(const PolicyArray& rhs, uint capacity,
                       AllocationMode am)
    : ArrayData(kPolicyKind, am)
    , Store(rhs, rhs.m_size, capacity, am, &rhs) {
  m_size = rhs.m_size;
  m_pos = rhs.m_pos;
  // Log at the end of the ctor so as to show the properly initialized
  // members.
  APILOG(this) << "(" << &rhs << ", " << capacity << ", " << uint(am) << ");";
}

PolicyArray::~PolicyArray() {
  APILOG(this) << "()";
  destroy(m_size, m_allocMode);
}

void PolicyArray::Release(ArrayData* ad) {
  asPolicyArray(ad)->release();
}

inline PolicyArray* PolicyArray::asPolicyArray(ArrayData* ad) {
  assert(ad->kind() == kPolicyKind);
  assert(dynamic_cast<PolicyArray*>(ad));
  return static_cast<PolicyArray*>(ad);
}

inline const PolicyArray* PolicyArray::asPolicyArray(const ArrayData* ad) {
  assert(ad->kind() == kPolicyKind);
  assert(dynamic_cast<const PolicyArray*>(ad));
  return static_cast<const PolicyArray*>(ad);
}

const Variant& PolicyArray::getValueRef(ssize_t pos) const {
  APILOG(this) << "(" << pos << ")";
  assert(size_t(pos) < m_size);
  return val(toPos(pos));
}

bool PolicyArray::isVectorData() const {
  APILOG(this) << "()";
  for (ssize_t i = 0; i < m_size; ++i) {
    if (Store::find(i, m_size) != toPos(i)) return false;
  }
  return true;
}

static_assert(ArrayData::invalid_index == size_t(-1), "ehm");

template <class K>
TypedValue* PolicyArray::nvGetImpl(K k) const {
  APILOG(this) << "(" << keystr(k) << ")";
  auto const pos = find(k, m_size);
  return LIKELY(pos != PosType::invalid)
    ? reinterpret_cast<TypedValue*>(&lval(pos))
    : nullptr;
}

TypedValue* PolicyArray::NvGetInt(const ArrayData* ad, int64_t k) {
  return asPolicyArray(ad)->nvGetImpl(k);
}

TypedValue* PolicyArray::NvGetStr(const ArrayData* ad, const StringData* k) {
  return asPolicyArray(ad)->nvGetImpl(k);
}

void PolicyArray::NvGetKey(const ArrayData* ad, TypedValue* out, ssize_t pos) {
  auto a = asPolicyArray(ad);
  APILOG(a) << "(" << out << ", " << pos << ")";
  assert(size_t(pos) < a->m_size);
  new(out) Variant(a->key(toPos(pos)));
}

template <class K>
ArrayData *PolicyArray::lvalImpl(K k, Variant*& ret, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << ret << ", "
         << copy << ", " << ")";

  if (copy) {
    return PolicyArray::copy()->lvalImpl(k, ret, false);
  }

  PosType pos = find(k, m_size);
  if (pos != PosType::invalid) {
    // found, don't overwrite anything
    assert(toInt<uint32_t>(pos) <= m_size);
    ret = &lval(pos);
    MYLOG << (void*)this << "->lvalImpl:" << "found at " << toInt<int64_t>(pos)
          << ", value=" << valstr(*ret) << ", size=" << m_size;
  } else {
    // not found, initialize
    if (m_size == capacity()) {
      grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
    }
    assert(m_size < capacity());
    ret = appendNoGrow(k, Variant::NullInit());
  }

  return this;
}

ArrayData *PolicyArray::lvalNew(Variant *&ret, bool copy) {
  if (copy) {
    return PolicyArray::copy()->lvalNew(ret, false);
  }

  // Andrei: TODO - append() currently never fails, probably it
  // should.
  auto oldSize = m_size;
  append(uninit_null(), false);
  assert(m_size == oldSize + 1);
  if (UNLIKELY(oldSize == m_size)) {
    ret = &Variant::lvalBlackHole();
  } else {
    assert(lastIndex(m_size) != PosType::invalid);
    ret = &lval(lastIndex(m_size));
  }

  return this;
}

ArrayData *PolicyArray::createLvalPtr(StringData* k, Variant *&ret, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << ret << ", " << copy << ")";
  return addLval(k, ret, copy);
}

ArrayData *PolicyArray::getLvalPtr(StringData* k, Variant *&ret, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << ret << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->getLvalPtr(k, ret, false);
  }
  const auto pos = find(k, m_size);
  ret = pos != PosType::invalid
    ? &Store::lval(pos)
    : nullptr;
  return this;
}

template <class K>
PolicyArray* PolicyArray::setImpl(K k, const Variant& v, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << valstr(v) << ", " << copy
         << ")";
  PolicyArray* result = this;
  if (copy) result = PolicyArray::copy();
  if (result->update(k, v, result->m_size, result->m_allocMode)) {
    // Added a new element, must update size and possibly m_pos
    if (m_pos == invalid_index) m_pos = result->m_size;
    result->m_size++;
  }
  return result;
}

ArrayData*
PolicyArray::SetInt(ArrayData* ad, int64_t k, CVarRef v, bool copy) {
  return asPolicyArray(ad)->setImpl(k, v, copy);
}

ArrayData*
PolicyArray::SetStr(ArrayData* ad, StringData* k, CVarRef v, bool copy) {
  return asPolicyArray(ad)->setImpl(k, v, copy);
}

template <class K>
ArrayData *PolicyArray::setRefImpl(K k, CVarRef v, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << valstr(v) << ", " << copy << ")";

  if (copy) {
    return PolicyArray::copy()->setRef(k, v, false);
  }

  auto const pos = find(k, m_size);
  assert(m_size <= capacity());
  if (pos != PosType::invalid) {
    // found, update
    lval(pos).assignRef(v);
  } else {
    // not found, create new element
    MYLOG << "setRef: not found, appending at " << m_size;
    if (m_size == capacity()) {
      MYLOG << "grow";
      grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
    }
    appendNoGrow(k, Variant::NoInit())->constructRefHelper(v);
  }
  return this;
}

template <class K>
ArrayData *PolicyArray::addImpl(K k, const Variant& v, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << valstr(v) << ", " << copy << ");";
  if (copy) {
    auto result = PolicyArray::copy(m_size * 2 + 1);
    result->add(k, v, false);
    return result;
  }
  assert(!exists(k));
  // Make sure there's enough capacity
  if (m_size == capacity()) {
    grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
  }
  appendNoGrow(k, v);
  return this;
}

template <class K>
PolicyArray *PolicyArray::addLvalImpl(K k, Variant*& ret, bool copy) {
  APILOG(this) << "(" << k << ", " << ret << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->addLval(k, ret, false);
  }
  assert(!exists(k) && m_size <= capacity());
  if (m_size == capacity()) {
    grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
  }
  ret = appendNoGrow(k, Variant::NullInit());
  MYLOG << (void*)this << "->lval:" << "added";
  return this;
}

template <class K>
ArrayData *PolicyArray::removeImpl(K k, bool copy) {
  APILOG(this) << "(" << keystr(k) << ", " << copy << ")";

  if (copy) {
    return PolicyArray::copy()->remove(k, false);
  }

  auto const pos = find(k, m_size);
  if (pos == PosType::invalid) {
    // Not found, nothing to delete
    MYLOG << "not found, nothing to delete: " << keystr(k);
    return this;
  }

  for (FullPosRange r(strongIterators()); !r.empty(); r.popFront()) {
    FullPos& fp = *r.front();
    if (ssize_t(pos) <= fp.m_pos) {
      // We are removing something before or at the current position,
      // back off position to account for the shifting.
      if (!fp.m_pos) fp.setResetFlag(true);
      else --fp.m_pos;
    }
  }

  Store::erase(pos, m_size);
  --m_size;

  if (!Store::before(m_pos, pos)) {
    // We removed something before or at the current position, back
    // off position to account for the shifting.
    m_pos = ssize_t(prevIndex(toPos(m_pos), m_size));
  }

  assert(size_t(m_pos) < m_size || m_pos == invalid_index);
  return this;
}

ssize_t PolicyArray::iter_begin() const {
  APILOG(this) << "()";
  return m_size ? toInt<int64_t>(firstIndex(m_size)) : invalid_index;
}

ssize_t PolicyArray::iter_end() const {
  APILOG(this) << "()";
  return ssize_t(lastIndex(m_size));
}

ssize_t PolicyArray::iter_advance(ssize_t prev) const {
  APILOG(this) << "(" << prev << ")";
  auto const result = toInt<int64_t>(nextIndex(toPos(prev), m_size));
  MYLOG << "returning " << result;
  return result;
}

ssize_t PolicyArray::iter_rewind(ssize_t prev) const {
  APILOG(this) << "(" << prev << ")";
  return toInt<int64_t>(prevIndex(toPos(prev), m_size));
}

bool PolicyArray::validFullPos(const FullPos& fp) const {
  APILOG(this) << "(" << fp.m_pos << ";" << fp.getResetFlag() << ")";
  assert(fp.getContainer() == this);
  return fp.m_pos != invalid_index;
}

bool PolicyArray::advanceFullPos(FullPos &fp) {
  APILOG(this) << "(" << fp.m_pos << ";" << fp.getResetFlag() << ")";
  assert(fp.getContainer() == this);
  if (fp.getResetFlag()) {
    fp.setResetFlag(false);
    fp.m_pos = invalid_index;
  } else if (fp.m_pos == invalid_index) {
    return false;
  }
  fp.m_pos = toInt<int64_t>(nextIndex(toPos(fp.m_pos), m_size));
  if (fp.m_pos == invalid_index) {
    return false;
  }
  // To conform to PHP behavior, we need to set the internal
  // cursor to point to the next element.
  m_pos = toInt<int64_t>(nextIndex(toPos(fp.m_pos), m_size));
  return true;
}

HphpArray* PolicyArray::toHphpArray() const {
  auto result = ArrayData::Make(m_size);
  FOR_EACH_RANGE (i, 0, m_size) {
    if (hasStrKey(toPos(i))) {
      result->add(key(toPos(i)).getStringData(), val(toPos(i)), false);
    } else {
      result->add(key(toPos(i)).getInt64(), val(toPos(i)), false);
    }
  }
  return result;
}

ArrayData* PolicyArray::escalateForSort() {
  APILOG(this) << "()";
  return toHphpArray();
}

PolicyArray *PolicyArray::copy() const {
  APILOG(this) << "()";
  auto result = NEW(PolicyArray)(
    *this,
    capacity() + (m_size == capacity()),
    m_allocMode);
  assert(result->getCount() == 0);
  return result;
}

PolicyArray* PolicyArray::copy(uint capacity) {
  APILOG(this) << "(" << capacity << ")";
  return NEW(PolicyArray)(*this, capacity, m_allocMode);
}

PolicyArray *PolicyArray::copyWithStrongIterators() const {
  APILOG(this) << "()";
  auto result = PolicyArray::copy();
  moveStrongIterators(result, const_cast<PolicyArray*>(this));
  assert(result->getCount() == 0);
  return result;
}

ArrayData *PolicyArray::nonSmartCopy() const {
  APILOG(this) << "()";
  //return NEW(PolicyArray)(*this, capacity(), true);
  return toHphpArray()->nonSmartCopy();
}

ArrayData *PolicyArray::Append(ArrayData* ad, const Variant& v, bool copy) {
  auto a = static_cast<PolicyArray*>(ad);
  APILOG(a) << "(" << valstr(v) << ", " << copy << ")";
  if (copy) a = a->PolicyArray::copy();
  a->grow(a->m_size, a->m_size + 1, a->m_size * 2 + 1, a->m_allocMode);
  a->appendNoGrow(a->nextKeyBump(), v);
  return a;
}

PolicyArray *PolicyArray::appendRef(const Variant& v, bool copy) {
  APILOG(this) << "(" << valstr(v) << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->appendRef(v, false);
  }
  //addValWithRef(nextKeyBump(), v);
  auto const k = nextKeyBump();
  if (m_size == capacity()) {
    grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
  }
  assert(m_size < capacity());
  appendNoGrow(k, Variant::NoInit())->constructRefHelper(v);
  return this;
}

  /**
   * Similar to append(v, copy), with reference in v preserved.
   */
ArrayData *PolicyArray::appendWithRef(CVarRef v, bool copy) {
  APILOG(this) << "(" << valstr(v) << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->appendWithRef(v, false);
  }
  if (m_size == capacity()) {
    grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
  }
  assert(m_size < capacity());
  appendNoGrow(nextKeyBump(), Variant::NullInit())->setWithRef(v);
  return this;
}

template <class K>
void PolicyArray::addValWithRef(K k, const Variant& v) {
  MYLOG << (void*)this << "->addValWithRef("
        << keystr(k) << ", " << valstr(v)
        << "); size=" << m_size;
  auto pos = find(k, m_size);
  if (pos != PosType::invalid) {
    return;
  }
  if (m_size == capacity()) {
    grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
  }
  assert(m_size < capacity());
  appendNoGrow(k, Variant::NullInit())->setWithRef(v);
}

void PolicyArray::nextInsertWithRef(const Variant& v) {
  MYLOG << (void*)this << "->nextInsertWithRef("
        << valstr(v)
        << "); size=" << m_size;
  // We need to define k here (before the if/grow) because otherwise
  // the overzealous gcc issues a spurious warning as such:
  //
  // hphp/runtime/base/policy_array.h: In member function 'void
  // HPHP::PolicyArray::nextInsertWithRef(const HPHP::Variant&)':
  // hphp/runtime/base/policy_array.h:114:5: error: assuming
  // signed overflow does not occur when assuming that (X + c) < X is
  // always false [-Werror=strict-overflow]
  auto const k = nextKeyBump();
  if (m_size == capacity()) {
    grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);
  }
  assert(m_size < capacity());
  appendNoGrow(k, Variant::NullInit())->setWithRef(v);
}

ArrayData *PolicyArray::plus(const ArrayData *elems, bool copy) {
  APILOG(this) << "(" << elems << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->plus(elems, false);
  }

  assert(elems);
  grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);

  for (ArrayIter it(elems); !it.end(); it.next()) {
    Variant key = it.first();
    const Variant& value = it.secondRef();
    if (key.isNumeric()) {
      addValWithRef(key.toInt64(), value);
    } else {
      addValWithRef(key.getStringData(), value);
    }
  }
  return this;
}

ArrayData *PolicyArray::merge(const ArrayData *elems, bool copy) {
  APILOG(this) << "(" << elems << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->merge(elems, false);
  }

  assert(elems);
  grow(m_size, m_size + 1, m_size * 2 + 1, m_allocMode);

  for (ArrayIter it(elems); !it.end(); it.next()) {
    Variant key = it.first();
    const Variant& value = it.secondRef();
    if (key.isNumeric()) {
      nextInsertWithRef(value);
    } else {
      StringData *s = key.getStringData();
      Variant *p;
      // Andrei TODO: make sure this is the right semantics
      lval(s, p, false);
      p->setWithRef(value);
    }
  }
  return this;
}

  /**
   * Stack function: pop the last item and return it.
   */
ArrayData* PolicyArray::pop(Variant &value) {
  APILOG(this) << "(" << &value << ")";
  if (getCount() > 1) {
    return PolicyArray::copy()->pop(value);
  }
  if (!m_size) {
    value = uninit_null();
    return this;
  }
  auto pos = lastIndex(m_size);
  assert(size_t(pos) < m_size);
  value = val(pos);

  // Match PHP 5.3.1 semantics
  if (!hasStrKey(pos)
      && Store::nextKey() == 1 + key(pos).toInt64()) {
    nextKeyPop();
  }

  Store::erase(pos, m_size);
  --m_size;
  // To match PHP-like semantics, the pop operation resets the array's
  // internal iterator.
  m_pos = m_size ? toInt<int64_t>(firstIndex(m_size)) : invalid_index;
  return this;
}

ArrayData *PolicyArray::dequeue(Variant &value) {
  APILOG(this) << "(" << &value << ")";
  if (getCount() > 1) {
    return PolicyArray::copy()->dequeue(value);
  }

  // To match PHP-like semantics, we invalidate all strong iterators when an
  // element is removed from the beginning of the array.
  freeStrongIterators();
  if (!m_size) {
    value = uninit_null();
    return this;
  }

  auto& front = lval(firstIndex(m_size));
  value = std::move(front);
  new(&front) Variant;
  erase(firstIndex(m_size), m_size);
  --m_size;
  renumber();

  // To match PHP-like semantics, the dequeue operation resets the array's
  // internal iterator
  m_pos = m_size ? toInt<int64_t>(firstIndex(m_size)) : invalid_index;
  return this;
}

ArrayData* PolicyArray::prepend(CVarRef v, bool copy) {
  APILOG(this) << "(" << valstr(v) << ", " << copy << ")";
  if (copy) {
    return PolicyArray::copy()->prepend(v, false);
  }
  // To match PHP-like semantics, we invalidate all strong iterators when an
  // element is added to the beginning of the array.
  freeStrongIterators();
  Store::prepend(v, m_size, m_allocMode);
  ++m_size;
  auto first = firstIndex(m_size);
  setKey(first, int64_t(0));
  // Renumber.
  renumber();
  // To match PHP-like semantics, the prepend operation resets the array's
  // internal iterator
  m_pos = toInt<int64_t>(first);
  return this;
}

void PolicyArray::renumber() {
  APILOG(this) << "()";
  if (!m_size) {
    return;
  }

  Variant currentPosKey;
  if (m_pos != invalid_index) {
    // Cache key for element associated with m_pos in order to update m_pos
    // below.
    assert(size_t(m_pos) < m_size);
    currentPosKey = key(toPos(m_pos));
  }

  vector<Variant> siKeys;
  for (FullPosRange r(strongIterators()); !r.empty(); r.popFront()) {
    auto const pos = toPos(r.front()->m_pos);
    if (pos != PosType::invalid) {
      siKeys.push_back(key(pos));
    }
  }
  nextKeyReset();
  FOR_EACH_RANGE (i, 0, m_size) {
    if (!hasStrKey(toPos(i))) {
      setKey(toPos(i), nextKeyBump());
    }
  }
  if (m_pos != invalid_index) {
    // Update m_pos, now that compaction is complete.
    if (currentPosKey.isString()) {
      m_pos = toInt<int64_t>(find(currentPosKey.getStringData(), m_size));
    } else if (currentPosKey.is(KindOfInt64)) {
      m_pos = toInt<int64_t>(find(currentPosKey.getInt64(), m_size));
    } else {
      assert(false);
    }
  }

  // Update strong iterators, now that compaction is complete.
  auto i = siKeys.cbegin();
  for (FullPosRange r(strongIterators()); !r.empty(); r.popFront()) {
    FullPos* fp = r.front();
    if (fp->m_pos == invalid_index) {
      continue;
    }
    auto& k = *i++;
    if (k.isString()) {
      fp->m_pos = toInt<int64_t>(find(k.getStringData(), m_size));
    } else {
      assert(k.is(KindOfInt64));
      fp->m_pos = toInt<int64_t>(find(k.getInt64(), m_size));
    }
  }
  assert(i == siKeys.cend());
}

void PolicyArray::onSetEvalScalar() {
  APILOG(this) << "()";
  //FOR_EACH_RANGE (pos, 0, m_size) {
  for (auto pos = firstIndex(m_size); pos != PosType::invalid;
       pos = nextIndex(pos, m_size)) {
    if (hasStrKey(pos)) {
      auto k = key(pos).getStringData();
      if (!k->isStatic()) {
        auto sk = StringData::GetStaticString(k);
        if (k->decRefCount() == 0) {
          DELETE(StringData)(k);
        }
        // Andrei TODO: inefficient, does one incref and then decref
        setKey(pos, sk);
        sk->decRefCount();
      }
    }
    lval(pos).setEvalScalar();
  }
}

ArrayData *PolicyArray::escalate() const {
  APILOG(this) << "()";
  return ArrayData::escalate();
}

} // namespace HPHP