#ifndef DELTA_FROM_C_H_
#define DELTA_FROM_C_H_

#include <algorithm>
#include <cmath>
#include <iostream>
#include <type_traits>

#include "galois/Galois.h"
#include "galois/Reduction.h"

/*
Derive delta from a graph-independent constant C:

    delta = C * mean_edge_weight / average_degree

and hand Galois the shift it actually wants, round(log2(delta)).

`average_degree` is the number of arcs stored per vertex, sizeEdges() / size().
An undirected graph is stored with every edge in both directions, so this is
2m/n, which is what the graph statistics tool reports as "Average degree".
`mean_edge_weight` is taken over the same stored arcs; an undirected edge
contributes twice with the same weight, so the mean is unaffected.

TIMING
------
DeltaSelector::Get() is called from inside the timed region of trial(), once per
round of every source, before any of the algorithm's own state is built. That is
the whole point: the cost of choosing delta is charged to the algorithm.

To take it back out of the timer, pass -delta-outside-timer. Warmup() then
computes the value once before the timer starts and Get() returns the cached
value. The call site never moves, so switching between the two costs nothing.

NOTE: UpdateRequestIndexer in Lonestar/BFS_SSSP.h caches 2^shift in a function
local `static` for floating-point weights. That is safe here only because the
derived shift is the same for every trial in a run (one graph, one C). Anything
that makes the shift vary within a process has to fix that cache first.
*/

template <typename GraphT_>
double MeanEdgeWeight(GraphT_& graph) {
  galois::GAccumulator<double> total;
  total.reset();

  galois::do_all(
      galois::iterate(graph),
      [&](typename GraphT_::GraphNode n) {
        for (auto e : graph.edges(n))
          total += static_cast<double>(graph.getEdgeData(e));
      },
      galois::steal(), galois::no_stats(), galois::loopname("MeanEdgeWeight"));

  return total.reduce() / static_cast<double>(graph.sizeEdges());
}

template <typename WeightT_>
class DeltaSelector {
public:
  DeltaSelector() = default;
  DeltaSelector(int fixed_shift, double c, bool use_c, bool outside_timer)
      : fixed_shift_(fixed_shift), c_(c), use_c_(use_c),
        outside_timer_(outside_timer) {}

  // Call before the timer starts. Only does work under -delta-outside-timer.
  template <typename GraphT_>
  void Warmup(GraphT_& graph) {
    if (use_c_ && outside_timer_) {
      cached_ = Compute(graph);
      have_cached_ = true;
    }
  }

  // Call inside the timed region, just before the distance array is built.
  // Returns the shift, which is what Galois' -delta takes.
  template <typename GraphT_>
  int Get(GraphT_& graph) const {
    if (!use_c_)
      last_ = fixed_shift_;
    else if (have_cached_)
      last_ = cached_;
    else
      last_ = Compute(graph);
    return last_;
  }

  // Report the shift the last Get() handed out. Call outside the timer.
  void PrintLast() const {
    if (use_c_)
      std::cout << "Derived delta: " << std::pow(2.0, last_) << " (shift "
                << last_ << ", C = " << c_ << ")" << std::endl;
  }

  bool use_c() const { return use_c_; }

private:
  template <typename GraphT_>
  int Compute(GraphT_& graph) const {
    const double average_degree = static_cast<double>(graph.sizeEdges()) /
                                  static_cast<double>(graph.size());
    const double delta = c_ * MeanEdgeWeight(graph) / average_degree;
    int shift = static_cast<int>(std::lround(std::log2(delta)));

    // Integer weights are bucketed with `dist >> shift`, so a negative shift
    // would be undefined; float weights divide by 2^shift and are fine.
    if (std::is_integral<WeightT_>::value)
      shift = std::max(0, shift);

    return shift;
  }

  int fixed_shift_ = 13;
  double c_ = 0.0;
  bool use_c_ = false;
  bool outside_timer_ = false;
  int cached_ = 0;
  bool have_cached_ = false;
  mutable int last_ = 0;
};

#endif // DELTA_FROM_C_H_
