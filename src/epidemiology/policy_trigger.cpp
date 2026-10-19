#include "epidemiology/policy_trigger.h"

#include <algorithm>

namespace june {

void PolicyTrigger::step(TriggerCell& cell, int64_t day, int64_t value) const {
  if (!cumulative) {
    if (!cell.closed && value >= activate_at) {
      cell.closed = true;
    } else if (cell.closed && value <= deactivate_at.value()) {
      cell.closed = false;
    }
    return;
  }

  if (!cell.closed) {
    if (value - cell.anchor_value >= activate_at) {
      cell.closed = true;
      cell.anchor_day = day;
      cell.anchor_value = value;
    }
    return;
  }

  const int64_t allowed =
      std::max(tolerance, tolerance_pct * cell.anchor_value / 100);
  if (value - cell.anchor_value > allowed) {
    cell.anchor_day = day;
    cell.anchor_value = value;
  } else if (day - cell.anchor_day >= calm_days) {
    cell.closed = false;
    cell.anchor_value = value;
  }
}

void PolicyTrigger::setUnitBit(VenueId unit, bool closed) {
  const size_t word = static_cast<size_t>(unit) / 64;
  const uint64_t bit = uint64_t{1} << (static_cast<size_t>(unit) % 64);
  if (closed) {
    if (word >= closed_bits_.size()) closed_bits_.resize(word + 1, 0);
    closed_bits_[word] |= bit;
  } else if (word < closed_bits_.size()) {
    closed_bits_[word] &= ~bit;
  }
}

bool PolicyTrigger::unitClosed(VenueId unit) const {
  if (unit < 0) return false;
  const size_t word = static_cast<size_t>(unit) / 64;
  return word < closed_bits_.size() &&
         (closed_bits_[word] >> (static_cast<size_t>(unit) % 64) & 1);
}

void PolicyTrigger::stepUnit(VenueId unit, int64_t day, int64_t value) {
  const auto it = cells_.find(unit);
  TriggerCell cell = it == cells_.end() ? TriggerCell{} : it->second;
  step(cell, day, value);
  setUnitBit(unit, cell.closed);
  if (cell == TriggerCell{}) {
    if (it != cells_.end()) cells_.erase(it);
  } else {
    cells_[unit] = cell;
  }
}

std::vector<VenueId> PolicyTrigger::closedUnits() const {
  std::vector<VenueId> out;
  for (const auto& [unit, cell] : cells_)
    if (cell.closed) out.push_back(unit);
  return out;
}

void PolicyTrigger::restoreUnits(
    const std::vector<std::pair<VenueId, TriggerCell>>& cells) {
  cells_.clear();
  closed_bits_.clear();
  for (const auto& [unit, cell] : cells) {
    cells_[unit] = cell;
    setUnitBit(unit, cell.closed);
  }
}

}  // namespace june
