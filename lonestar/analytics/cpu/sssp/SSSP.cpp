/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/Galois.h"
#include "galois/AtomicHelpers.h"
#include "galois/Reduction.h"
#include "galois/PriorityQueue.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "Lonestar/BoilerPlate.h"
#include "Lonestar/BFS_SSSP.h"
#include "Lonestar/Utils.h"

#include "llvm/Support/CommandLine.h"

#include <iostream>

namespace cll = llvm::cl;

static const char* name = "Single Source Shortest Path";
static const char* desc =
    "Computes the shortest path from a source node to all nodes in a directed "
    "graph using a modified chaotic iteration algorithm";
static const char* url = "single_source_shortest_path";

static cll::opt<std::string>
    inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<unsigned int>
    startNode("startNode",
              cll::desc("Node to start search from (default value 0)"),
              cll::init(0));
static cll::opt<unsigned int>
    reportNode("reportNode",
               cll::desc("Node to report distance to(default value 1)"),
               cll::init(1));
static cll::opt<int>
    stepShift("delta",
              cll::desc("Shift value for the deltastep (default value 13)"),
              cll::init(13));
static cll::opt<unsigned int>
    sources("sources", cll::desc("Number of sources to test (default value 1)"),
            cll::init(1));
static cll::opt<unsigned int>
    rounds("rounds", cll::desc("Number of rounds to test (default value 22)"),
           cll::init(22));

enum Algo {
  deltaTile = 0,
  deltaStep,
  deltaStepBarrier,
  serDeltaTile,
  serDelta,
  dijkstraTile,
  dijkstra,
  topo,
  topoTile,
  AutoAlgo
};

const char* const ALGO_NAMES[] = {
    "deltaTile", "deltaStep",    "deltaStepBarrier", "serDeltaTile",
    "serDelta",  "dijkstraTile", "dijkstra",         "topo",
    "topoTile",  "Auto"};

static cll::opt<Algo> algo(
    "algo", cll::desc("Choose an algorithm (default value auto):"),
    cll::values(clEnumVal(deltaTile, "deltaTile"),
                clEnumVal(deltaStep, "deltaStep"),
                clEnumVal(deltaStepBarrier, "deltaStepBarrier"),
                clEnumVal(serDeltaTile, "serDeltaTile"),
                clEnumVal(serDelta, "serDelta"),
                clEnumVal(dijkstraTile, "dijkstraTile"),
                clEnumVal(dijkstra, "dijkstra"), clEnumVal(topo, "topo"),
                clEnumVal(topoTile, "topoTile"),
                clEnumVal(AutoAlgo,
                          "auto: choose among the algorithms automatically")),
    cll::init(AutoAlgo));

#ifdef USE_FLOAT
typedef float weight_type;
#else
typedef uint32_t weight_type;
#endif

//! [withnumaalloc]
using Graph =
    galois::graphs::LC_CSR_Graph<std::atomic<weight_type>, weight_type>::
        with_no_lockable<true>::type ::with_numa_alloc<true>::type;
//! [withnumaalloc]
typedef Graph::GraphNode GNode;

constexpr static const bool TRACK_WORK          = false;
constexpr static const unsigned CHUNK_SIZE      = 128U;
constexpr static const ptrdiff_t EDGE_TILE_SIZE = 512;

using SSSP                 = BFS_SSSP<Graph, weight_type, true, EDGE_TILE_SIZE>;
using Dist                 = SSSP::Dist;
using UpdateRequest        = SSSP::UpdateRequest;
using UpdateRequestIndexer = SSSP::UpdateRequestIndexer;
using SrcEdgeTile          = SSSP::SrcEdgeTile;
using SrcEdgeTileMaker     = SSSP::SrcEdgeTileMaker;
using SrcEdgeTilePushWrap  = SSSP::SrcEdgeTilePushWrap;
using ReqPushWrap          = SSSP::ReqPushWrap;
using OutEdgeRangeFn       = SSSP::OutEdgeRangeFn;
using TileRangeFn          = SSSP::TileRangeFn;

namespace gwl = galois::worklists;
using PSchunk = gwl::PerSocketChunkFIFO<CHUNK_SIZE>;
using OBIM    = gwl::OrderedByIntegerMetric<UpdateRequestIndexer, PSchunk>;
using OBIM_Barrier =
    gwl::OrderedByIntegerMetric<UpdateRequestIndexer,
                                PSchunk>::with_barrier<true>::type;
#ifdef COUNT_RELAX
std::atomic<size_t> Relaxations;
#endif

template <typename T, typename OBIMTy = OBIM, typename P, typename R>
void deltaStepAlgo(Graph& graph, GNode source, const P& pushWrap,
                   const R& edgeRange) {
#ifdef COUNT_RELAX
  Relaxations = 0;
#endif

  //! [reducible for self-defined stats]
  galois::GAccumulator<size_t> BadWork;
  //! [reducible for self-defined stats]
  galois::GAccumulator<size_t> WLEmptyWork;

  graph.getData(source) = 0;

  galois::InsertBag<T> initBag;
  pushWrap(initBag, source, 0, "parallel");

  galois::for_each(
      galois::iterate(initBag),
      [&](const T& item, auto& ctx) {
        constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;
        const auto& sdata                 = graph.getData(item.src, flag);

        if (sdata < item.dist) {
          if (TRACK_WORK)
            WLEmptyWork += 1;
          return;
        }

        for (auto ii : edgeRange(item)) {

          GNode dst          = graph.getEdgeDst(ii);
          auto& ddist        = graph.getData(dst, flag);
          Dist ew            = graph.getEdgeData(ii, flag);
          const Dist newDist = sdata + ew;
          Dist oldDist       = galois::atomicMin<weight_type>(ddist, newDist);
#ifdef COUNT_RELAX
          Relaxations++;
#endif
          if (newDist < oldDist) {
            if (TRACK_WORK) {
              //! [per-thread contribution of self-defined stats]
              if (oldDist != SSSP::DIST_INFINITY) {
                BadWork += 1;
              }
              //! [per-thread contribution of self-defined stats]
            }
            pushWrap(ctx, dst, newDist);
          }
        }
      },
      galois::wl<OBIMTy>(UpdateRequestIndexer{stepShift}),
      galois::disable_conflict_detection(), galois::loopname("SSSP"));

  if (TRACK_WORK) {
    //! [report self-defined stats]
    galois::runtime::reportStat_Single("SSSP", "BadWork", BadWork.reduce());
    //! [report self-defined stats]
    galois::runtime::reportStat_Single("SSSP", "WLEmptyWork",
                                       WLEmptyWork.reduce());
  }
}

template <typename T, typename P, typename R>
void serDeltaAlgo(Graph& graph, const GNode& source, const P& pushWrap,
                  const R& edgeRange) {

  SerialBucketWL<T, UpdateRequestIndexer> wl(UpdateRequestIndexer{stepShift});
  ;
  graph.getData(source) = 0;

  pushWrap(wl, source, 0);

  size_t iter = 0UL;
  while (!wl.empty()) {

    auto& curr = wl.minBucket();

    while (!curr.empty()) {
      ++iter;
      auto item = curr.front();
      curr.pop_front();

      if (graph.getData(item.src) < item.dist) {
        // empty work
        continue;
      }

      for (auto e : edgeRange(item)) {

        GNode dst   = graph.getEdgeDst(e);
        auto& ddata = graph.getData(dst);

        const auto newDist = item.dist + graph.getEdgeData(e);

        if (newDist < ddata) {
          ddata = newDist;
          pushWrap(wl, dst, newDist);
        }
      }
    }

    wl.goToNextBucket();
  }

  if (!wl.allEmpty()) {
    std::abort();
  }
  galois::runtime::reportStat_Single("SSSP-Serial-Delta", "Iterations", iter);
}

template <typename T, typename P, typename R>
void dijkstraAlgo(Graph& graph, const GNode& source, const P& pushWrap,
                  const R& edgeRange) {

  using WL = galois::MinHeap<T>;

  graph.getData(source) = 0;

  WL wl;
  pushWrap(wl, source, 0);

  size_t iter = 0;

  while (!wl.empty()) {
    ++iter;

    T item = wl.pop();

    if (graph.getData(item.src) < item.dist) {
      // empty work
      continue;
    }

    for (auto e : edgeRange(item)) {

      GNode dst   = graph.getEdgeDst(e);
      auto& ddata = graph.getData(dst);

      const auto newDist = item.dist + graph.getEdgeData(e);

      if (newDist < ddata) {
        ddata = newDist;
        pushWrap(wl, dst, newDist);
      }
    }
  }

  galois::runtime::reportStat_Single("SSSP-Dijkstra", "Iterations", iter);
}

void topoAlgo(Graph& graph, const GNode& source) {

  galois::LargeArray<Dist> oldDist;
  oldDist.allocateInterleaved(graph.size());

  constexpr Dist INFTY = SSSP::DIST_INFINITY;
  galois::do_all(
      galois::iterate(size_t{0}, graph.size()),
      [&](size_t i) { oldDist.constructAt(i, INFTY); }, galois::no_stats(),
      galois::loopname("initDistArray"));

  graph.getData(source) = 0;

  galois::GReduceLogicalOr changed;
  size_t rounds = 0;

  do {

    ++rounds;
    changed.reset();

    galois::do_all(
        galois::iterate(graph),
        [&](const GNode& n) {
          const auto& sdata = graph.getData(n);

          if (oldDist[n] > sdata) {

            oldDist[n] = sdata;
            changed.update(true);

            for (auto e : graph.edges(n)) {
              const auto newDist = sdata + graph.getEdgeData(e);
              auto dst           = graph.getEdgeDst(e);
              auto& ddata        = graph.getData(dst);
              galois::atomicMin(ddata, newDist);
            }
          }
        },
        galois::steal(), galois::loopname("Update"));

  } while (changed.reduce());

  galois::runtime::reportStat_Single("SSSP-topo", "rounds", rounds);
}

void topoTileAlgo(Graph& graph, const GNode& source) {

  galois::InsertBag<SrcEdgeTile> tiles;

  graph.getData(source) = 0;

  galois::do_all(
      galois::iterate(graph),
      [&](const GNode& n) {
        SSSP::pushEdgeTiles(tiles, graph, n,
                            SrcEdgeTileMaker{n, SSSP::DIST_INFINITY});
      },
      galois::steal(), galois::loopname("MakeTiles"));

  galois::GReduceLogicalOr changed;
  size_t rounds = 0;

  do {
    ++rounds;
    changed.reset();

    galois::do_all(
        galois::iterate(tiles),
        [&](SrcEdgeTile& t) {
          const auto& sdata = graph.getData(t.src);

          if (t.dist > sdata) {

            t.dist = sdata;
            changed.update(true);

            for (auto e = t.beg; e != t.end; ++e) {
              const auto newDist = sdata + graph.getEdgeData(e);
              auto dst           = graph.getEdgeDst(e);
              auto& ddata        = graph.getData(dst);
              galois::atomicMin(ddata, newDist);
            }
          }
        },
        galois::steal(), galois::loopname("Update"));

  } while (changed.reduce());

  galois::runtime::reportStat_Single("SSSP-topo", "rounds", rounds);
}

void trial(Graph& graph, GNode source) {
  galois::do_all(galois::iterate(graph),
                 [&graph](GNode n) { graph.getData(n) = SSSP::DIST_INFINITY; });

  graph.getData(source) = 0;

  std::cout << "Running " << ALGO_NAMES[algo] << " algorithm\n";

  galois::StatTimer autoAlgoTimer("AutoAlgo_0");
  galois::StatTimer execTime("Timer_0");
  execTime.start();

  if (algo == AutoAlgo) {
    autoAlgoTimer.start();
    if (isApproximateDegreeDistributionPowerLaw(graph)) {
      algo = deltaStep;
    } else {
      algo = deltaStepBarrier;
    }
    autoAlgoTimer.stop();
    galois::gInfo("Choosing ", ALGO_NAMES[algo], " algorithm");
  }

  switch (algo) {
  case deltaTile:
    deltaStepAlgo<SrcEdgeTile>(graph, source, SrcEdgeTilePushWrap{graph},
                               TileRangeFn());
    break;
  case deltaStep:
    deltaStepAlgo<UpdateRequest>(graph, source, ReqPushWrap(),
                                 OutEdgeRangeFn{graph});
    break;
  case serDeltaTile:
    serDeltaAlgo<SrcEdgeTile>(graph, source, SrcEdgeTilePushWrap{graph},
                              TileRangeFn());
    break;
  case serDelta:
    serDeltaAlgo<UpdateRequest>(graph, source, ReqPushWrap(),
                                OutEdgeRangeFn{graph});
    break;
  case dijkstraTile:
    dijkstraAlgo<SrcEdgeTile>(graph, source, SrcEdgeTilePushWrap{graph},
                              TileRangeFn());
    break;
  case dijkstra:
    dijkstraAlgo<UpdateRequest>(graph, source, ReqPushWrap(),
                                OutEdgeRangeFn{graph});
    break;
  case topo:
    topoAlgo(graph, source);
    break;
  case topoTile:
    topoTileAlgo(graph, source);
    break;

  case deltaStepBarrier:
    deltaStepAlgo<UpdateRequest, OBIM_Barrier>(graph, source, ReqPushWrap(),
                                               OutEdgeRangeFn{graph});
    break;

  default:
    std::abort();
  }

  execTime.stop();

  std::cout << "Galois execution time: " << (double)execTime.get_usec() / 1e6
            << "s" << std::endl;

  // Sanity checking code
  galois::GReduceMax<weight_type> maxDistance;
  galois::GAccumulator<weight_type> distanceSum;
  galois::GAccumulator<uint32_t> visitedNode;
  maxDistance.reset();
  distanceSum.reset();
  visitedNode.reset();

  galois::do_all(
      galois::iterate(graph),
      [&](weight_type i) {
        weight_type myDistance = graph.getData(i);

        if (myDistance != SSSP::DIST_INFINITY) {
          maxDistance.update(myDistance);
          distanceSum += myDistance;
          visitedNode += 1;
        }
      },
      galois::loopname("Sanity check"), galois::no_stats());

  // report sanity stats
  weight_type rMaxDistance = maxDistance.reduce();
  // uint64_t rDistanceSum = distanceSum.reduce();
  uint64_t rVisitedNode = visitedNode.reduce();

#ifdef COUNT_RELAX
  std::cout << "Number of relaxations: " << Relaxations << std::endl;
#endif
  galois::gInfo("# visited nodes is ", rVisitedNode);
  galois::gInfo("Max distance is ", rMaxDistance);

  if (!skipVerify) {
    if (SSSP::verify(graph, source)) {
      std::cout << "Verification successful.\n";
    } else {
      GALOIS_DIE("verification failed");
    }
  }
}

template <typename NodeID_, typename rng_t_,
          typename uNodeID_ = typename std::make_unsigned<NodeID_>::type>
class UniDist {
public:
  UniDist(NodeID_ max_value, rng_t_& rng) : rng_(rng) {
    no_mod_                  = rng_.max() == static_cast<uNodeID_>(max_value);
    mod_                     = max_value + 1;
    uNodeID_ remainder_sub_1 = rng_.max() % mod_;
    if (remainder_sub_1 == mod_ - 1)
      cutoff_ = 0;
    else
      cutoff_ = rng_.max() - remainder_sub_1;
  }

  NodeID_ operator()() {
    uNodeID_ rand_num = rng_();
    if (no_mod_)
      return rand_num;
    if (cutoff_ != 0) {
      while (rand_num >= cutoff_)
        rand_num = rng_();
    }
    return rand_num % mod_;
  }

private:
  rng_t_& rng_;
  bool no_mod_;
  uNodeID_ mod_;
  uNodeID_ cutoff_;
};

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url, &inputFile);

  Graph graph;

  std::cout << "Reading from file: " << inputFile << "\n";
  galois::graphs::readGraph(graph, inputFile);
  std::cout << "Read " << graph.size() << " nodes, " << graph.sizeEdges()
            << " edges\n";

  if (startNode >= graph.size()) {
    std::cerr << "failed to set source: " << startNode << "\n";
    assert(0);
    abort();
  }

  auto it = graph.begin();
  it      = graph.begin();

  size_t approxNodeData = graph.size() * 64;
  galois::preAlloc(numThreads +
                   approxNodeData / galois::runtime::pagePoolSize());

  std::mt19937_64 rng(27491095);
  UniDist<GNode, std::mt19937_64> udist(graph.size() - 1, rng);

  for (unsigned int v = 0; v < sources.getValue(); v++) {
    GNode s;
    uint64_t deg;

    do {
      s   = udist();
      deg = graph.getDegree(s);

    } while (deg == 0);

    std::cout << "source = " << s << std::endl;
    for (unsigned int i = 0; i <= rounds.getValue(); i++) {
      trial(graph, s);
    }
  }
  return 0;
}
