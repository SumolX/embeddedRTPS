/**
 * Copyright © 2019 Lehrstuhl Informatik 11 - RWTH Aachen University
 * 
 * This file is part of embeddedRTPS.
 * 
 * You should have received a copy of the MIT License along with embeddedRTPS.
 * If not, see <https://mit-license.org>.
 */

#pragma once

#include <array>
#include <stdint.h>

namespace rtps {

/**
 * Extension of the SimpleHistoryCache that allows for deletion operation at the
 * cost of efficieny
 * TODO: Replace with something better in the future!
 * Likely only used for SEDP
 */
template <uint16_t SIZE> class HistoryCacheWithDeletion {
public:
  HistoryCacheWithDeletion() = default;

  uint32_t m_dispose_after_write_cnt = 0;

  bool isFull() const {
    uint16_t it = m_head;
    incrementIterator(it);
    return it == m_tail;
  }

  const CacheChange *addChange(const uint8_t *data, DataSize_t size,
                               bool inLineQoS, bool disposeAfterWrite) {
    CacheChange change;
    change.kind = ChangeKind_t::ALIVE;
    change.inLineQoS = inLineQoS;
    change.disposeAfterWrite = disposeAfterWrite;
    change.data.reserve(size);
    change.data.append(data, size);
    change.sequenceNumber = ++m_lastUsedSequenceNumber;

    if (disposeAfterWrite) {
      m_dispose_after_write_cnt++;
    }

    CacheChange *place = &m_buffer[m_head];
    incrementHead();

    *place = std::move(change);
    return place;
  }

  const CacheChange *addChange(const uint8_t *data, DataSize_t size) {
    return addChange(data, size, 0, false);
  }

  void removeUntilIncl(SequenceNumber_t sn) {
    if (m_head == m_tail) {
      return;
    }

    if (getCurrentSeqNumMax() <= sn) { // We won't overrun head
      m_head = m_tail;
      return;
    }

    while (m_buffer[m_tail].sequenceNumber <= sn) {
      incrementTail();
    }
  }

  void dropOldest() { removeUntilIncl(getCurrentSeqNumMin()); }

  bool dropChange(const SequenceNumber_t &sn) {
    uint16_t idx_to_clear;
    CacheChange *change;
    if (!getChangeBySN(sn, &change, idx_to_clear)) {
      return false; // sn does not exist, nothing to do
    }

    if (idx_to_clear == m_tail) {
      m_buffer[m_tail].reset();
      incrementTail();
      return true;
    }

    uint16_t prev = idx_to_clear;
    do {
      prev = idx_to_clear - 1;
      if (prev >= m_buffer.size()) {
        prev = m_buffer.size() - 1;
      }

      m_buffer[idx_to_clear] = std::move(m_buffer[prev]);
      idx_to_clear = prev;

    } while (prev != m_tail);

    incrementTail();

    return true;
  }

  bool setCacheChangeKind(const SequenceNumber_t &sn, ChangeKind_t kind) {
    CacheChange *change = getChangeBySN(sn);
    if (change == nullptr) {
      return false;
    }

    change->kind = kind;
    return true;
  }

  CacheChange *getChangeBySN(SequenceNumber_t sn) {
    CacheChange *change;
    uint16_t position;
    if (getChangeBySN(sn, &change, position)) {
      return change;
    } else {
      return nullptr;
    }
  }

  bool isEmpty() { return (m_head == m_tail); }

  const SequenceNumber_t &getCurrentSeqNumMin() const {
    if (m_head == m_tail) {
      return SEQUENCENUMBER_UNKNOWN;
    } else {
      return m_buffer[m_tail].sequenceNumber;
    }
  }

  const SequenceNumber_t &getCurrentSeqNumMax() const {
    if (m_head == m_tail) {
      return SEQUENCENUMBER_UNKNOWN;
    } else {
      return m_lastUsedSequenceNumber;
    }
  }

  const SequenceNumber_t &getLastUsedSequenceNumber() {
    return m_lastUsedSequenceNumber;
  }

  void clear() {
    m_head = 0;
    m_tail = 0;
    m_lastUsedSequenceNumber = {0, 0};
  }
#ifdef DEBUG_HISTORY_CACHE_WITH_DELETION
  void print() {
    for (unsigned int i = 0; i < m_buffer.size(); i++) {
      std::cout << "[" << i << "] "
                << " SN = " << m_buffer[i].sequenceNumber.low;
      switch (m_buffer[i].kind) {
      case ChangeKind_t::ALIVE:
        std::cout << " Type = ALIVE";
        break;
      case ChangeKind_t::INVALID:
        std::cout << " Type = INVALID";
        break;
      case ChangeKind_t::NOT_ALIVE_DISPOSED:
        std::cout << " Type = DISPOSED";
        break;
      }
      if (m_head == i) {
        std::cout << " <- HEAD";
      }
      if (m_tail == i) {
        std::cout << " <- TAIL";
      }
      std::cout << std::endl;
    }
  }
#endif
  bool isSNInRange(const SequenceNumber_t &sn) {
    if (isEmpty()) {
      return false;
    }
    SequenceNumber_t minSN = getCurrentSeqNumMin();
    if (sn < minSN || getCurrentSeqNumMax() < sn) {
      return false;
    }
    return true;
  }

private:
  std::array<CacheChange, SIZE + 1> m_buffer{};
  uint16_t m_head = 0;
  uint16_t m_tail = 0;
  static_assert(sizeof(SIZE) <= sizeof(m_head),
                "Iterator is large enough for given size");

  SequenceNumber_t m_lastUsedSequenceNumber{0, 0};

  bool getChangeBySN(const SequenceNumber_t &sn, CacheChange **out_change,
                     uint16_t &out_buffer_position) {
    if (!isSNInRange(sn)) {
      return false;
    }
    static_assert(std::is_unsigned<decltype(sn.low)>::value,
                  "Underflow well defined");
    static_assert(sizeof(m_tail) <= sizeof(uint16_t), "Cast ist well defined");

    unsigned int cur_idx = m_tail;
    while (cur_idx != m_head) {
      if (m_buffer[cur_idx].sequenceNumber == sn) {
        *out_change = &m_buffer[cur_idx];
        out_buffer_position = cur_idx;
        return true;
      }
      // Sequence numbers are consecutive
      if (m_buffer[cur_idx].sequenceNumber > sn) {
        *out_change = nullptr;
        return false;
      }

      cur_idx++;
      if (cur_idx >= m_buffer.size()) {
        cur_idx -= m_buffer.size();
      }
    }

    *out_change = nullptr;
    return false;
  }

  inline void incrementHead() {
    incrementIterator(m_head);
    if (m_head == m_tail) {
      // Move without check
      incrementIterator(m_tail); // drop one
    }
  }

  inline void incrementIterator(uint16_t &iterator) const {
    ++iterator;
    if (iterator >= m_buffer.size()) {
      iterator = 0;
    }
  }

  inline void incrementTail() {
    if (m_buffer[m_tail].disposeAfterWrite) {
      m_dispose_after_write_cnt--;
    }
    if (m_head != m_tail) {
      m_buffer[m_tail].reset();
      incrementIterator(m_tail);
    }
  }

protected:
  // This constructor was created for unit testing
  explicit HistoryCacheWithDeletion(SequenceNumber_t lastUsed)
      : HistoryCacheWithDeletion() {
    m_lastUsedSequenceNumber = lastUsed;
  }
};

}
