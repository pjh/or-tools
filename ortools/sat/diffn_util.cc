// Copyright 2010-2021 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/sat/diffn_util.h"

#include "absl/algorithm/container.h"
#include "ortools/base/stl_util.h"

namespace operations_research {
namespace sat {

bool Rectangle::IsDisjoint(const Rectangle& other) const {
  return x_min >= other.x_max || other.x_min >= x_max || y_min >= other.y_max ||
         other.y_min >= y_max;
}

std::vector<absl::Span<int>> GetOverlappingRectangleComponents(
    const std::vector<Rectangle>& rectangles,
    absl::Span<int> active_rectangles) {
  if (active_rectangles.empty()) return {};

  std::vector<absl::Span<int>> result;
  const int size = active_rectangles.size();
  for (int start = 0; start < size;) {
    // Find the component of active_rectangles[start].
    int end = start + 1;
    for (int i = start; i < end; i++) {
      for (int j = end; j < size; ++j) {
        if (!rectangles[active_rectangles[i]].IsDisjoint(
                rectangles[active_rectangles[j]])) {
          std::swap(active_rectangles[end++], active_rectangles[j]);
        }
      }
    }
    if (end > start + 1) {
      result.push_back(active_rectangles.subspan(start, end - start));
    }
    start = end;
  }
  return result;
}

bool ReportEnergyConflict(Rectangle bounding_box, absl::Span<const int> boxes,
                          SchedulingConstraintHelper* x,
                          SchedulingConstraintHelper* y) {
  x->ClearReason();
  y->ClearReason();
  IntegerValue total_energy(0);
  for (const int b : boxes) {
    const IntegerValue x_min = x->ShiftedStartMin(b);
    const IntegerValue x_max = x->EndMax(b);
    if (x_min < bounding_box.x_min || x_max > bounding_box.x_max) continue;
    const IntegerValue y_min = y->ShiftedStartMin(b);
    const IntegerValue y_max = y->EndMax(b);
    if (y_min < bounding_box.y_min || y_max > bounding_box.y_max) continue;

    x->AddEnergyAfterReason(b, x->SizeMin(b), bounding_box.x_min);
    x->AddEndMaxReason(b, bounding_box.x_max);
    y->AddEnergyAfterReason(b, y->SizeMin(b), bounding_box.y_min);
    y->AddEndMaxReason(b, bounding_box.y_max);

    total_energy += x->SizeMin(b) * y->SizeMin(b);

    // We abort early if a subset of boxes is enough.
    // TODO(user): Also relax the box if possible.
    if (total_energy > bounding_box.Area()) break;
  }

  CHECK_GT(total_energy, bounding_box.Area());
  x->ImportOtherReasons(*y);
  return x->ReportConflict();
}

bool BoxesAreInEnergyConflict(const std::vector<Rectangle>& rectangles,
                              const std::vector<IntegerValue>& energies,
                              absl::Span<const int> boxes,
                              Rectangle* conflict) {
  // First consider all relevant intervals along the x axis.
  std::vector<IntegerValue> x_starts;
  std::vector<TaskTime> boxes_by_increasing_x_max;
  for (const int b : boxes) {
    x_starts.push_back(rectangles[b].x_min);
    boxes_by_increasing_x_max.push_back({b, rectangles[b].x_max});
  }
  gtl::STLSortAndRemoveDuplicates(&x_starts);
  std::sort(boxes_by_increasing_x_max.begin(), boxes_by_increasing_x_max.end());

  std::vector<IntegerValue> y_starts;
  std::vector<IntegerValue> energy_sum;
  std::vector<TaskTime> boxes_by_increasing_y_max;

  std::vector<std::vector<int>> stripes(x_starts.size());
  for (int i = 0; i < boxes_by_increasing_x_max.size(); ++i) {
    const int b = boxes_by_increasing_x_max[i].task_index;
    const IntegerValue x_min = rectangles[b].x_min;
    const IntegerValue x_max = rectangles[b].x_max;
    for (int j = 0; j < x_starts.size(); ++j) {
      if (x_starts[j] > x_min) break;
      stripes[j].push_back(b);

      // Redo the same on the y coordinate for the current x interval
      // which is [starts[j], x_max].
      y_starts.clear();
      boxes_by_increasing_y_max.clear();
      for (const int b : stripes[j]) {
        y_starts.push_back(rectangles[b].y_min);
        boxes_by_increasing_y_max.push_back({b, rectangles[b].y_max});
      }
      gtl::STLSortAndRemoveDuplicates(&y_starts);
      std::sort(boxes_by_increasing_y_max.begin(),
                boxes_by_increasing_y_max.end());

      const IntegerValue x_size = x_max - x_starts[j];
      energy_sum.assign(y_starts.size(), IntegerValue(0));
      for (int i = 0; i < boxes_by_increasing_y_max.size(); ++i) {
        const int b = boxes_by_increasing_y_max[i].task_index;
        const IntegerValue y_min = rectangles[b].y_min;
        const IntegerValue y_max = rectangles[b].y_max;
        for (int j = 0; j < y_starts.size(); ++j) {
          if (y_starts[j] > y_min) break;
          energy_sum[j] += energies[b];
          if (energy_sum[j] > x_size * (y_max - y_starts[j])) {
            if (conflict != nullptr) {
              *conflict = rectangles[b];
              for (int k = 0; k < i; ++k) {
                const int task_index = boxes_by_increasing_y_max[k].task_index;
                if (rectangles[task_index].y_min >= y_starts[j]) {
                  conflict->TakeUnionWith(rectangles[task_index]);
                }
              }
            }
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool AnalyzeIntervals(bool transpose, absl::Span<const int> local_boxes,
                      const std::vector<Rectangle>& rectangles,
                      const std::vector<IntegerValue>& rectangle_energies,
                      IntegerValue* x_threshold, IntegerValue* y_threshold,
                      Rectangle* conflict) {
  // First, we compute the possible x_min values (removing duplicates).
  // We also sort the relevant tasks by their x_max.
  //
  // TODO(user): If the number of unique x_max is smaller than the number of
  // unique x_min, it is better to do it the other way around.
  std::vector<IntegerValue> starts;
  std::vector<TaskTime> task_by_increasing_x_max;
  for (const int t : local_boxes) {
    const IntegerValue x_min =
        transpose ? rectangles[t].y_min : rectangles[t].x_min;
    const IntegerValue x_max =
        transpose ? rectangles[t].y_max : rectangles[t].x_max;
    starts.push_back(x_min);
    task_by_increasing_x_max.push_back({t, x_max});
  }
  gtl::STLSortAndRemoveDuplicates(&starts);

  // Note that for the same end_max, the order change our heuristic to
  // evaluate the max_conflict_height.
  std::sort(task_by_increasing_x_max.begin(), task_by_increasing_x_max.end());

  // The maximum y dimension of a bounding area for which there is a potential
  // conflict.
  IntegerValue max_conflict_height(0);

  // This is currently only used for logging.
  absl::flat_hash_set<std::pair<IntegerValue, IntegerValue>> stripes;

  // All quantities at index j correspond to the interval [starts[j], x_max].
  std::vector<IntegerValue> energies(starts.size(), IntegerValue(0));
  std::vector<IntegerValue> y_mins(starts.size(), kMaxIntegerValue);
  std::vector<IntegerValue> y_maxs(starts.size(), -kMaxIntegerValue);
  std::vector<IntegerValue> energy_at_max_y(starts.size(), IntegerValue(0));
  std::vector<IntegerValue> energy_at_min_y(starts.size(), IntegerValue(0));

  // Sentinel.
  starts.push_back(kMaxIntegerValue);

  // Iterate over all boxes by increasing x_max values.
  int first_j = 0;
  const IntegerValue threshold = transpose ? *y_threshold : *x_threshold;
  for (int i = 0; i < task_by_increasing_x_max.size(); ++i) {
    const int t = task_by_increasing_x_max[i].task_index;

    const IntegerValue energy = rectangle_energies[t];
    IntegerValue x_min = rectangles[t].x_min;
    IntegerValue x_max = rectangles[t].x_max;
    IntegerValue y_min = rectangles[t].y_min;
    IntegerValue y_max = rectangles[t].y_max;
    if (transpose) {
      std::swap(x_min, y_min);
      std::swap(x_max, y_max);
    }

    // Add this box contribution to all the [starts[j], x_max] intervals.
    while (first_j + 1 < starts.size() && x_max - starts[first_j] > threshold) {
      ++first_j;
    }
    for (int j = first_j; starts[j] <= x_min; ++j) {
      const IntegerValue old_energy_at_max = energy_at_max_y[j];
      const IntegerValue old_energy_at_min = energy_at_min_y[j];

      energies[j] += energy;

      const bool is_disjoint = y_min >= y_maxs[j] || y_max <= y_mins[j];

      if (y_min <= y_mins[j]) {
        if (y_min < y_mins[j]) {
          y_mins[j] = y_min;
          energy_at_min_y[j] = energy;
        } else {
          energy_at_min_y[j] += energy;
        }
      }

      if (y_max >= y_maxs[j]) {
        if (y_max > y_maxs[j]) {
          y_maxs[j] = y_max;
          energy_at_max_y[j] = energy;
        } else {
          energy_at_max_y[j] += energy;
        }
      }

      // If the new box is disjoint in y from the ones added so far, there
      // cannot be a new conflict involving this box, so we skip until we add
      // new boxes.
      if (is_disjoint) continue;

      const IntegerValue width = x_max - starts[j];
      IntegerValue conflict_height = CeilRatio(energies[j], width) - 1;
      if (y_max - y_min > conflict_height) continue;
      if (conflict_height >= y_maxs[j] - y_mins[j]) {
        // We have a conflict.
        if (conflict != nullptr) {
          *conflict = rectangles[t];
          for (int k = 0; k < i; ++k) {
            const int task_index = task_by_increasing_x_max[k].task_index;
            const IntegerValue task_x_min = transpose
                                                ? rectangles[task_index].y_min
                                                : rectangles[task_index].x_min;
            if (task_x_min < starts[j]) continue;
            conflict->TakeUnionWith(rectangles[task_index]);
          }
        }
        return false;
      }

      // Because we currently do not have a conflict involving the new box, the
      // only way to have one is to remove enough energy to reduce the y domain.
      IntegerValue can_remove = std::min(old_energy_at_min, old_energy_at_max);
      if (old_energy_at_min < old_energy_at_max) {
        if (y_maxs[j] - y_min >=
            CeilRatio(energies[j] - old_energy_at_min, width)) {
          // In this case, we need to remove at least old_energy_at_max to have
          // a conflict.
          can_remove = old_energy_at_max;
        }
      } else if (old_energy_at_max < old_energy_at_min) {
        if (y_max - y_mins[j] >=
            CeilRatio(energies[j] - old_energy_at_max, width)) {
          can_remove = old_energy_at_min;
        }
      }
      conflict_height = CeilRatio(energies[j] - can_remove, width) - 1;

      // If the new box height is above the conflict_height, do not count
      // it now. We only need to consider conflict involving the new box.
      if (y_max - y_min > conflict_height) continue;

      if (VLOG_IS_ON(2)) stripes.insert({starts[j], x_max});
      max_conflict_height = std::max(max_conflict_height, conflict_height);
    }
  }

  VLOG(2) << " num_starts: " << starts.size() - 1 << "/" << local_boxes.size()
          << " conflict_height: " << max_conflict_height
          << " num_stripes:" << stripes.size() << " (<= " << threshold << ")";

  if (transpose) {
    *x_threshold = std::min(*x_threshold, max_conflict_height);
  } else {
    *y_threshold = std::min(*y_threshold, max_conflict_height);
  }
  return true;
}

absl::Span<int> FilterBoxesAndRandomize(
    const std::vector<Rectangle>& cached_rectangles, absl::Span<int> boxes,
    IntegerValue threshold_x, IntegerValue threshold_y,
    absl::BitGenRef random) {
  size_t new_size = 0;
  for (const int b : boxes) {
    const Rectangle& dim = cached_rectangles[b];
    if (dim.x_max - dim.x_min > threshold_x) continue;
    if (dim.y_max - dim.y_min > threshold_y) continue;
    boxes[new_size++] = b;
  }
  if (new_size == 0) return {};
  std::shuffle(&boxes[0], &boxes[0] + new_size, random);
  return {&boxes[0], new_size};
}

}  // namespace sat
}  // namespace operations_research
