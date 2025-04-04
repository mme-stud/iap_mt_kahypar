
#pragma once

#include <atomic>
#include <mutex>

#include <tbb/parallel_invoke.h>

#include "kahypar-resources/meta/mandatory.h"

#include "mt-kahypar/datastructures/partitioned_hypergraph.h"
#include "mt-kahypar/datastructures/priority_queue.h"

namespace mt_kahypar {
namespace ds {

template <typename Numerator, typename Denominator = Numerator>
class NonnegativeFraction {
private:
  Numerator numerator;
  Denominator denominator;
public:
  // infinity per default
  NonnegativeFraction() :
    numerator(1),
    denominator(0) { }

  NonnegativeFraction(const Numerator& n, const Denominator& d) :
    numerator(n),
    denominator(d) { 
      ASSERT(d >= 0);
      ASSERT(n >= 0);
    }

  bool operator< (const NonnegativeFraction& other) const {
    size_t lhs = static_cast<size_t>(numerator) * static_cast<size_t>(other.denominator);
    size_t rhs = static_cast<size_t>(other.numerator) * static_cast<size_t>(denominator);
    return lhs < rhs;
  }

  bool operator== (const NonnegativeFraction& other) const {
    size_t lhs = static_cast<size_t>(numerator) * static_cast<size_t>(other.denominator);
    size_t rhs = static_cast<size_t>(other.numerator) * static_cast<size_t>(denominator);
    return lhs == rhs;
  }

  bool operator> (const NonnegativeFraction& other) const {
    // case "this = 0" or "other = infinity"
    if (numerator == 0 || other.denominator == 0) return false;
    // case "this = infinity" or "other = 0"
    if (denominator == 0 || other.numerator == 0) return true;

    // !!! Now we have no zero and no infinity
    // cases whith big difference
    if (denominator / numerator < other.denominator / other.numerator) {
      return true;
    }
    if (numerator / denominator > other.numerator / other.denominator) {
      return true;
    }

    // case "this <= other"
    if (numerator <= other.numerator && denominator >= other.denominator) { 
      return false;
    }
    // case "this > other" if **not** both numerators and denominators are equal 
    if (numerator >= other.numerator && denominator <= other.denominator) {
      return numerator != other.numerator || denominator != other.denominator;
    }

    // case default
    uintmax_t lhs = static_cast<uintmax_t>(numerator) * static_cast<uintmax_t>(other.denominator);
    uintmax_t rhs = static_cast<uintmax_t>(other.numerator) * static_cast<uintmax_t>(denominator);
    return lhs > rhs;
  }

  // operator<< 
  friend std::ostream& operator<< (std::ostream& s, const NonnegativeFraction& f) {
    return s << f.numerator << " / " << f.denominator;
  }

  // Numerator must be non-negative 
  void setNumerator(const Numerator& n) {
    ASSERT(n >= 0);
    numerator = n;
  }
  // ! Denominator must be greater than 0
  void setDenominator(const Denominator& d) {
    ASSERT(d >= 0);
    denominator = d;
  }
  Numerator getNumerator() const {
    return numerator;
  }
  Denominator getDenominator() const {
    return denominator;
  }

  double_t value() const {
    if (denominator == 0) {
      return std::numeric_limits<double_t>::max();
    }
    return static_cast<double_t>(numerator) / static_cast<double_t>(denominator);
  }
};

using ConductanceFraction = NonnegativeFraction<HypergraphVolume>;


/**
 * @brief Priority Queue for partitions based on their conductance.
 * 
 * All write operations are synchronized.
 * All read operations aren't synchronized per default (use the synchronized flag to synchronize).
 */
template <typename PartitionedHypergraph = Mandatory>
class ConductancePriorityQueue : 
      protected ExclusiveHandleHeap<MaxHeap<ConductanceFraction, PartitionID>> {
private:
  using SuperPQ = ExclusiveHandleHeap<MaxHeap<ConductanceFraction, PartitionID>>;
public:
  ConductancePriorityQueue() :
    SuperPQ(0),
    _total_volume(-1),
    _size(0),
    _complement_val_bits(),
    _initialized(false)
    { }
  
  // ! Initializes the priority queue with the partitions of the hypergraph
  // ! Could be called concurrently !!!
  void initialize(const PartitionedHypergraph& hg, bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::initialize(hg, " << synchronized << ")" << std::endl;
    // ASSERT(!_initialized); could be called concurrently -> no assertion before lock
    lock(synchronized);
    if (_initialized) {
      unlock(synchronized);
      return;
    }
    _total_volume = getHGTotalVolume(hg);
    _size = hg.k();
    _complement_val_bits.resize(_size);
    SuperPQ::clear();
    SuperPQ::resize(_size);
    SuperPQ::heap.resize(_size);
    tbb::parallel_for(PartitionID(0), _size, [&](const PartitionID& p) {
      HypergraphVolume cut_weight = getHGPartCutWeight(hg, p);
      HypergraphVolume part_volume = getHGPartVolume(hg, p);
      _complement_val_bits[p] = (part_volume > _total_volume - part_volume);
      ConductanceFraction f(cut_weight, std::min(part_volume, _total_volume - part_volume));
      SuperPQ::heap[p].id = p; 
      SuperPQ::heap[p].key = f;
      SuperPQ::positions[p] = p;
    });
    buildHeap();
    _initialized = true;
    unlock(synchronized);
  }

  // ! Returns true if the priority queue is initialized
  bool initialized() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::initialized()" << std::endl;
    return _initialized;
  }

  // ! Reset the priority queue to the uninitialized state
  void reset(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::reset()" << std::endl;
    lock(synchronized);
    SuperPQ::clear();
    _total_volume = -1;
    _size = 0;
    _complement_val_bits.clear();
    _initialized = false;
    unlock(synchronized);
  }

  // ! Returns an approximate memory consumption of the conductance priority queue in bytes
  size_t memoryConsumption() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::memoryConsumption()" << std::endl;
    return SuperPQ::memoryConsumption() + _complement_val_bits.size() * sizeof(bool);
  }

  // ! Updates the priority queue after global changes in partition
  void globalUpdate(const PartitionedHypergraph& hg, bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::globalUpdate(hg, " << synchronized << ")" << std::endl;
    lock(synchronized);
    ASSERT(_initialized && _size == hg.k());
    if (_uses_original_stats) {
      ASSERT(_total_volume == hg.originalTotalVolume(), "Total volume in ConductancePriorityQueue is" << _total_volume << ", but should be" << hg.originalTotalVolume());
    }
    _total_volume = getHGTotalVolume(hg);
    tbb::parallel_for(PartitionID(0), _size, [&](const PartitionID& p) {
      HypergraphVolume cut_weight = getHGPartCutWeight(hg, p);
      HypergraphVolume part_volume = getHGPartVolume(hg, p);
      ASSERT(part_volume <= _total_volume, "Partition volume" << part_volume << "is greater than total volume" << _total_volume);
      ASSERT(cut_weight <= part_volume, "Cut weight" << cut_weight << "is greater than partition volume" << part_volume);
      ASSERT(cut_weight + part_volume <= _total_volume, "Cut weight" << cut_weight << "and partition volume" << part_volume << "is greater than total volume" << _total_volume);
      _complement_val_bits[p] = (part_volume > _total_volume - part_volume);
      ConductanceFraction f(cut_weight, std::min(part_volume, _total_volume - part_volume));
      SuperPQ::heap[SuperPQ::positions[p]].key = f;
    });
    buildHeap();
    unlock(synchronized);
  }

  // ! Checks if the priority queue is correct with respect to the hypergraph: const version
  bool check(const PartitionedHypergraph& hg) const {
    /// [debug] std::cerr << "ConductancePriorityQueue::check(hg)" << std::endl;
    ASSERT(_initialized && _size == hg.k());
    bool correct = true;
    if (_total_volume != getHGTotalVolume(hg)) {
      correct = false;
      LOG << "Total volume in ConductancePriorityQueue is" << _total_volume << ", but should be" << getHGTotalVolume(hg);
    }
    for (PartitionID p = 0; p < _size; ++p) {
      ConductanceFraction f = SuperPQ::getKey(p);
      HypergraphVolume cut_weight = f.getNumerator();
      HypergraphVolume part_volume = f.getDenominator();
      ASSERT(part_volume <= _total_volume);
      ASSERT(cut_weight <= part_volume);
      if (_complement_val_bits[p]) {
        part_volume = _total_volume - part_volume;
      }
      ASSERT(cut_weight <= part_volume);
      if (part_volume != getHGPartVolume(hg, p)) {
        correct = false;
        LOG << "Volume of partition in ConductancePriorityQueue" << p << "is" << part_volume << ", but should be" << getHGPartVolume(hg, p);
      }
      if (cut_weight != getHGPartCutWeight(hg, p)) {
        correct = false;
        LOG << "Cut weight of partition in ConductancePriorityQueue" << p << "is" << cut_weight << ", but should be" << getHGPartCutWeight(hg, p);
      }
    }
    correct = correct && SuperPQ::isHeap() && SuperPQ::positionsMatch();
    return correct;
  }

  // ! Checks if the priority queue is correct with respect to the hypergraph: synchronizable version
  bool checkSync(const PartitionedHypergraph& hg, bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::checkSync(hg, " << synchronized << ")" << std::endl;
    lock(synchronized);
    bool correct = check(hg);
    unlock(synchronized);
    return correct;
  }

  // ################# Priority Queue Operations #################

  bool isHeap() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::isHeap(" << synchronized << ")" << std::endl;
    for (PosT i = 1; i < size(); ++i) {
      if (heap[i].key > heap[parent(i)].key) {
        return false;
      }
    }
    return true;
  }

  // ! Adjusts the cut weight and the volume of a partition
  // ! changes pq => uses a lock  
  void adjustKey(const PartitionID& p, const HypergraphVolume& cut_weight, const HypergraphVolume& part_volume, bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::adjustKey(" << p << ", " << cut_weight << ", " << part_volume << ", " << synchronized << ")" << std::endl;
    ASSERT(_initialized);
    ASSERT(static_cast<size_t>(_size) == _complement_val_bits.size());
    if (part_volume < cut_weight || _total_volume < part_volume || _total_volume < part_volume + cut_weight) {
      // this update is incorrect (potentially due to concurrency) => will be redone later
      // [used by changeNodePart, where only for the last thread changing partition p 
      //                                  the right stats are guaranteed]
      LOG << "ConductancePriorityQueue::adjustKey(p = " << p << ", cut_weight = " 
          << cut_weight << ", part_volume = " << part_volume << ") is skipped due to incorrect stats: "
          << "total_volume = " << _total_volume << ". "
          << "[shouldn't be a problem]";
      return;
    }
    lock(synchronized);
    // LOG << "ConductancePriorityQueue::adjustKey(p = " << p << ", cut_weight = " 
    // << cut_weight << ", part_volume = " << part_volume << ") is started";
    _complement_val_bits[p] = (part_volume > _total_volume - part_volume);
    ConductanceFraction f(cut_weight, std::min(part_volume, _total_volume - part_volume));
    SuperPQ::adjustKey(p, f);
    SuperPQ::heap[SuperPQ::positions[p]].key = f; // needed, as the fraction are equal if their reduced forms are equal
    // so SuperPQ::adjustKey(p, f) would not change the key to the new one
    // but in ConductancePriorityQueue we need to set the key to the exact numerator and denominator
    // as they have meaning in the context of the hypergraph
    // LOG << "ConductancePriorityQueue::adjustKey(p = " << p << ", cut_weight = " 
    // << cut_weight << ", part_volume = " << part_volume << ") is finished";
    unlock(synchronized);
  }
  
  // ! Updates PQ after total volume of the hypergraph has changed
  // ! changes pq => uses a lock
  void updateTotalVolume(const HypergraphVolume& new_total_volume, bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::updateTotalVolume(" << new_total_volume << ", " << synchronized << ")" << std::endl;		
    ASSERT(_initialized && static_cast<size_t>(_size) == _complement_val_bits.size());
    lock(synchronized);
    for (PartitionID p = 0; p < _size; ++p) {
      ConductanceFraction& f = SuperPQ::getKey(p);
      HypergraphVolume part_volume = f.getDenominator();
      ASSERT(part_volume <= _total_volume && part_volume <= new_total_volume);
      if (_complement_val_bits[p]) {
        part_volume = _total_volume - part_volume;
      }
      _complement_val_bits[p] = (part_volume > new_total_volume - part_volume);
      f.setDenominator(std::min(part_volume, new_total_volume - part_volume));
    }
    buildHeap();
    _total_volume = new_total_volume;
    unlock(synchronized);
  }

  // ! Get the partition with the highest conductance: const version
  PartitionID top() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::top()" << std::endl;
    PartitionID p = SuperPQ::top();
    return p;
  }

  // ! Get the partition with the highest conductance: synchronizable version
  PartitionID topSync(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::topSync(" << synchronized << ")" << std::endl;
    lock(synchronized);
    PartitionID p = top();
    unlock(synchronized);
    return p;
  }

  // ! Get the partition with the second highest conductance: const version
  PartitionID secondTop() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::secondTop()" << std::endl;
    ASSERT(SuperPQ::size() > 1);
    PartitionID f = SuperPQ::heap[1].id;
    // ConductancePriorityQueue is a MaxHeap => binary tree
    if (size() > 2 && SuperPQ::heap[1].key < SuperPQ::heap[2].key) {
      f = SuperPQ::heap[2].id;
    }
    return f;
  }
  
  // ! Get the partition with the second highest conductance: synchronizable version
  PartitionID secondTopSync(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::secondTopSync(" << synchronized << ")" << std::endl;
    lock(synchronized);
    PartitionID f = secondTop();
    unlock(synchronized);
    return f;
  }

  // ! Get the top three partitions (unsorted): constant version
  // ! (Works only for a binary heap)
  vec<PartitionID> topThree() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::topThree()" << std::endl;
    vec<PartitionID> top_three(3, kInvalidPartition);
    for (size_t i = 0; i < 3 && i < _size; ++i) {
      top_three[i] = SuperPQ::heap[i].id;
    }
    return top_three;
  }

  // ! Get the top three partitions (unsorted): synchronizable version
  // ! (Works only for a binary heap)
  vec<PartitionID> topThreeSync(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::topThreeSync(" << synchronized << ")" << std::endl;
    lock(synchronized);
    vec<PartitionID> top_three = topThree();
    unlock(synchronized);
    return top_three;
  }
  
  bool empty() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::empty()" << std::endl;
    bool e = SuperPQ::empty();
    return e;
  }

  size_t size() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::size()" << std::endl;
    ASSERT(static_cast<PosT>(_size) == SuperPQ::size());
    return _size;
  }

  // ################## USAGE OF ORIGINAL PHG STATS #################

  // ! Uses the original stats of the hypergraph
  bool usesOriginalStats() const {
    /// [debug] std::cerr << "ConductancePriorityQueue::usesOriginalStats()" << std::endl;
    return _uses_original_stats;
  }
  
  // ! Makes ConductancePriorityQueue use current _total_volume and _part_volumes
  // ! instead of the original (inherited from old versions of hg) ones
  // ! To be used before initialization
  void disableUsageOfOriginalHGStats() {
    /// [debug] std::cerr << "ConductancePriorityQueue::disableUsageOfOriginalHGStats()" << std::endl;
    ASSERT(!_initialized, "ConductancePriorityQueue is already initialized");
    _uses_original_stats = false;
  }

  // ! Makes ConductancePriorityQueue use original _total_volume and _part_volumes
  // ! instead of the current ones
  // ! To be used before initialization
  void enableUsageOfOriginalHGStats() {
    /// [debug] std::cerr << "ConductancePriorityQueue::enableUsageOfOriginalHGStats()" << std::endl;
    ASSERT(!_initialized, "ConductancePriorityQueue is already initialized");
    _uses_original_stats = true;
  }

  // ################# MUTEX OPERATIONS FOR CHANGING PQ #################

  // ! Exclude simultaneous synchronized access to the priority queue
  // ! To be used when HG is changed and the priority queue is changed in parallel
  void lock(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::lock(synchronized)" << std::endl;
    if (synchronized) _pq_lock.lock();
  }

  void unlock(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::unlock(synchronized)" << std::endl;
    if (synchronized) _pq_lock.unlock();
  }

private:
  // ! Builds the heap in O(_size) time
  // ! no built in lock
  void buildHeap() {
    /// [debug] std::cerr << "ConductancePriorityQueue::buildHeap()" << std::endl;
    ASSERT(static_cast<PosT>(_size) == SuperPQ::size());
    if (isHeap()) return;
    for (PartitionID p = _size - 1; p >= 0; --p) {
      SuperPQ::siftDown(p);
    }
    ASSERT(SuperPQ::isHeap() && SuperPQ::positionsMatch());
  }

  // ################### COMMUNICATION WITH THE HG ######################
  // ! Get needed kind of total volume
  // ! (original or current)
  HypergraphVolume getHGTotalVolume(const PartitionedHypergraph& hg) const {
    /// [debug] std::cerr << "ConductancePriorityQueue::getHGTotalVolume(hg)" << std::endl;
    if (_uses_original_stats) {
      return hg.originalTotalVolume();
    } else {
      return hg.totalVolume();
    }
  }
  
  // ! Get needed kind of part volume
  // ! (original or current)
  HypergraphVolume getHGPartVolume(const PartitionedHypergraph& hg, const PartitionID p) const {
    /// [debug] std::cerr << "ConductancePriorityQueue::getHGPartVolume(hg, " << p << ")" << std::endl;
    if (_uses_original_stats) {
      return hg.partOriginalVolume(p);
    } else {
      return hg.partVolume(p);
    }
  }

  // ! Get needed kind of cut weight
  // ! (only one kind of cut weight for now)
  HypergraphVolume getHGPartCutWeight(const PartitionedHypergraph& hg, const PartitionID p) const {
    /// [debug] std::cerr << "ConductancePriorityQueue::getHGPartCutWeight(hg, " << p << ")" << std::endl;
    return hg.partCutWeight(p);
  }

  // #################### POTENTIALLY USELESS PART ######################
  
  // ! insert a partition with its cut weight and volume
  // ! changes pq => uses a lock
  // ! not sure if this is useful
  void insert(const PartitionID& p, const HypergraphVolume& cut_weight, const HypergraphVolume& volume, bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::insert(" << p << ", " << cut_weight << ", " << volume << ", " << synchronized << ")" << std::endl;
    ConductanceFraction f(cut_weight, std::min(volume, _total_volume - volume));
    lock(synchronized);
    ASSERT(_total_volume >= volume);
    _complement_val_bits[p] = (volume > _total_volume - volume);
    SuperPQ::insert(p, f);
    _size++;
    unlock(synchronized);
  }

  // ! changes pq => uses a lock
  // ! not sure if this is useful
  void remove(const PartitionID& p, bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::remove(" << p << ", " << synchronized << ")" << std::endl;
    lock(synchronized);
    SuperPQ::remove(p);
    _size--;
    unlock(synchronized);
  }


  // ! not sure if this is useful
  void deleteTop(bool synchronized = true) {
    /// [debug] std::cerr << "ConductancePriorityQueue::deleteTop(" << synchronized << ")" << std::endl;
    lock(synchronized);
    SuperPQ::deleteTop();
    _size--;
    unlock(synchronized);
  }

  // ! Get the conductance fraction of the partition with the highest conductance
  ConductanceFraction topFractionSync(bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::topFractionSync(" << synchronized << ")" << std::endl;
    lock(synchronized);
    ConductanceFraction f = SuperPQ::topKey();
    unlock(synchronized);
    return f;
  }
  // ! Get the conductance fraction of the partition with the second highest conductance
  ConductanceFraction secondTopFractionSync(bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::secondTopFractionSync(" << synchronized << ")" << std::endl;
    ASSERT(SuperPQ::size() > 1);
    lock(synchronized);
    ConductanceFraction f =  SuperPQ::heap[1].key;
    // ConductancePriorityQueue is a MaxHeap => binary tree
    if (SuperPQ::size() > 2) {
      f = std::max(f, SuperPQ::heap[2].key);
    }
    unlock(synchronized);
    return f;
  }

  double_t topConductanceSync(bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::topConductanceSync(" << synchronized << ")" << std::endl;
    ConductanceFraction f = topFractionSync(synchronized);
    return f.value();
  }
  double_t secondTopCondunctanceSync(bool synchronized = false) {
    // [debug] std::cerr << "ConductancePriorityQueue::secondTopConductanceSync(" << synchronized << ")" << std::endl;
    ConductanceFraction f = secondTopFractionSync(synchronized);
    return f.value();
  }

  // ! Get the conductance fraction of a partition
  // (better use PartitionedHypergraph getPartCutWeight and getPartVolume)
  ConductanceFraction getFractionSync(const PartitionID p, bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::getFractionSync(" << p << ", " << synchronized << ")" << std::endl;
    lock(synchronized);
    ConductanceFraction f = SuperPQ::getKey(p);
    unlock(synchronized);
    return f;
  }
  // ! Get the conductance of a partition
  // (better use PartitionedHypergraph getPartCutWeight and getPartVolume)
  double_t getConductanceSync(const PartitionID p, bool synchronized = false) {
    /// [debug] std::cerr << "ConductancePriorityQueue::getConductanceSync(" << p << ", " << synchronized << ")" << std::endl;
    lock(synchronized);
    ConductanceFraction f = SuperPQ::getKey(p);
    unlock(synchronized);
    return f.value();
  }

  // ################# MEMBER VARIABLES #################
  SpinLock _pq_lock;
  HypergraphVolume _total_volume;
  PartitionID _size;
  vec<bool> _complement_val_bits;
  bool _uses_original_stats = true;
  bool _initialized;
};


}  // namespace ds
}  // namespace mt_kahypar