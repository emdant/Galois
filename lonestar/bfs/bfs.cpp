/** Breadth-first search -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
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
 *
 * @section Description
 *
 * Breadth-first search.
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 * @author M. Amber Hassaan <m.a.hassaan@utexas.edu>
 */
#include "galois/Galois.h"
#include "galois/gstl.h"
#include "galois/Reduction.h"
#include "galois/Timer.h"
#include "galois/Timer.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "llvm/Support/CommandLine.h"

#include "Lonestar/BoilerPlate.h"

#include "Lonestar/BFS_SSSP.h"

#include <iostream>
#include <deque>
#include <type_traits>

namespace cll = llvm::cl;

static const char* name = "Breadth-first Search";

static const char* desc =
  "Computes the shortest path from a source node to all nodes in a directed "
  "graph using a modified Bellman-Ford algorithm";

static const char* url = "breadth_first_search";

static cll::opt<std::string> filename(cll::Positional, 
                                      cll::desc("<input graph>"), 
                                      cll::Required);

static cll::opt<unsigned int> startNode("startNode",
                                        cll::desc("Node to start search from"),
                                        cll::init(0));
static cll::opt<unsigned int> reportNode("reportNode", 
                                         cll::desc("Node to report distance to"),
                                         cll::init(1));
static cll::opt<int> stepShift("delta",
                               cll::desc("Shift value for the deltastep"),
                               cll::init(10));
enum Algo {
  Async,
  Sync2p,
  Sync,
  SerialSync,
  Serial
};

static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumVal(Async, "Async"),
      clEnumVal(Sync2p, "Sync2p"),
      clEnumVal(Sync, "Sync"),
      clEnumVal(Serial, "Serial"),
      clEnumVal(SerialSync, "SerialSync"),
      clEnumValEnd), cll::init(Async));


using Graph = galois::graphs::LC_CSR_Graph<unsigned, void>
  ::with_no_lockable<true>::type
  ::with_numa_alloc<true>::type;

using GNode =  Graph::GraphNode;

constexpr static const bool TRACK_WORK = false;
constexpr static const unsigned CHUNK_SIZE = 256u;
constexpr static const ptrdiff_t EDGE_TILE_SIZE = 256;

using BFS = BFS_SSSP<Graph, unsigned int, EDGE_TILE_SIZE>;

struct EdgeTile {
  Graph::edge_iterator beg;
  Graph::edge_iterator end;
};

struct EdgeTileMaker {
  EdgeTile operator () (Graph::edge_iterator beg, Graph::edge_iterator end) const {
    return EdgeTile{ beg, end };
  }
};

void sync2phaseAlgo(Graph& graph, GNode source) {


  constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  BFS::Dist nextLevel = 0u;
  graph.getData(source, flag) = 0u;

  galois::InsertBag<GNode> activeNodes;
  galois::InsertBag<EdgeTile> edgeTiles;

  activeNodes.push(source);
  EdgeTileMaker etm;

  while (!activeNodes.empty()) {

    galois::do_all(galois::iterate(activeNodes),
        [&] (const GNode& src) {

          BFS::pushEdgeTiles(edgeTiles, graph, src, etm);
        
        },
        galois::steal(),
        galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("activeNodes"));

    ++nextLevel;
    activeNodes.clear_parallel();

    galois::do_all(galois::iterate(edgeTiles),
        [&] (const EdgeTile& tile) {

          for (auto e = tile.beg; e != tile.end; ++e) {
            auto dst = graph.getEdgeDst(e);
            auto& dstData = graph.getData(dst, flag);

            if (dstData == BFS::DIST_INFINITY) {
              dstData = nextLevel;
              activeNodes.push(dst);
            }
          }
        },
        galois::steal(),
        galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("edgeTiles"));

    edgeTiles.clear_parallel();
  }
}

void syncAlgo(Graph& graph, GNode source) {

  using Bag = galois::InsertBag<EdgeTile>;
  constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  Bag* curr = new Bag();
  Bag* next = new Bag();

  BFS::Dist nextLevel = 0u;
  graph.getData(source, flag) = 0u;

  EdgeTileMaker etm;

  BFS::pushEdgeTilesParallel(*next, graph, source, etm);
  assert(!next->empty());

  while (!next->empty()) {

    std::swap(curr, next);
    next->clear_parallel();
    ++nextLevel;

    galois::do_all(galois::iterate(*curr),
        [&] (const EdgeTile& tile) {

          for (auto e = tile.beg; e != tile.end; ++e) {
            auto dst = graph.getEdgeDst(e);
            auto& dstData = graph.getData(dst, flag);

            if (dstData == BFS::DIST_INFINITY) {
              dstData = nextLevel;
              BFS::pushEdgeTiles(*next, graph, dst, etm);
            }
          }
        },
        galois::steal(),
        galois::chunk_size<CHUNK_SIZE>(),
        galois::loopname("Sync"));
  }


  delete curr;
  delete next;
}

void asyncAlgo(Graph& graph, GNode source) {


  namespace gwl = galois::worklists;
  //typedef dChunkedFIFO<CHUNK_SIZE> dFIFO;
  using FIFO = gwl::dChunkedFIFO<CHUNK_SIZE>;
  using BSWL =  gwl::BulkSynchronous< gwl::dChunkedLIFO<CHUNK_SIZE> >;
  using WL = BSWL;

  constexpr bool useCAS = !std::is_same<WL, BSWL>::value;

  galois::GAccumulator<size_t> BadWork;
  galois::GAccumulator<size_t> WLEmptyWork;

  graph.getData(source) = 0;
  galois::InsertBag<BFS::DistEdgeTile> initBag;

  BFS::pushEdgeTilesParallel(initBag, graph, source, BFS::DistEdgeTileMaker {1});

  galois::for_each(galois::iterate(initBag)
      , [&] (const BFS::DistEdgeTile& tile, auto& ctx) {

        constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

        auto newDist = tile.dist;

        for (auto ii = tile.beg; ii != tile.end; ++ii) {
          GNode dst = graph.getEdgeDst(ii);
          auto& ddata  = graph.getData(dst, flag);

          while (true) {

            auto oldDist = ddata;

            if (oldDist <= newDist) {
              break;
            }

            if (!useCAS || __sync_bool_compare_and_swap(&ddata, oldDist, newDist)) {

              if (!useCAS) {
                ddata = newDist;
              }

              BFS::pushEdgeTiles(ctx, graph, dst, BFS::DistEdgeTileMaker {newDist} );
              break;
            }
          }
        } // end for
      }
      , galois::wl<WL>()
      , galois::loopname("runBFS")
      , galois::no_conflicts());

  if (TRACK_WORK) {
    galois::runtime::reportStat_Single("BFS", "BadWork", BadWork.reduce());
    galois::runtime::reportStat_Single("BFS", "EmptyWork", WLEmptyWork.reduce());
  }
}

void serialAlgo(Graph& graph, GNode source) {

  using Req = BFS::UpdateRequest;
  using WL = std::deque<Req>;
  constexpr galois::MethodFlag flag = galois::MethodFlag::UNPROTECTED;

  WL wl;

  graph.getData(source, flag) = 0;
  wl.push_back( Req(source, 1) );

  size_t iter = 0;

  while (!wl.empty()) {
    ++iter;

    Req req = wl.front();
    wl.pop_front();

    for (auto e: graph.edges(req.n, flag)) {

      auto dst = graph.getEdgeDst(e);
      auto& dstData = graph.getData(dst, flag);

      if (dstData == BFS::DIST_INFINITY) {
        dstData = req.w;
        wl.push_back( Req(dst, req.w + 1) );
      }
    }
  }

  galois::runtime::reportStat_Single("BFS-Serial", "Iterations", iter);
}

void serialSyncAlgo(Graph& graph, GNode source) {
  using WL = std::vector<EdgeTile>;

  WL* curr = new WL();
  WL* next = new WL();

  size_t iter = 0;

  graph.getData(source) = 0;
  BFS::Dist nextLevel = 0;

  BFS::pushEdgeTiles(*next, graph, source, EdgeTileMaker());

  while (!next->empty()) {

    std::swap(curr, next);
    next->clear();
    ++nextLevel;

    iter += curr->size();

    for (const EdgeTile& t: *curr) {

      for (auto e = t.beg; e != t.end; ++e) {
        auto dst = graph.getEdgeDst(e);
        auto& dstData = graph.getData(dst);

        if (dstData == BFS::DIST_INFINITY) {
          dstData = nextLevel;
          BFS::pushEdgeTiles(*next, graph, dst, EdgeTileMaker());
        }
      }
    }
  }


  delete curr;
  delete next;

  galois::runtime::reportStat_Single("BFS-Serial", "Iterations", iter);
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url);

  galois::StatTimer T("OverheadTime");
  T.start();
  
  Graph graph;
  GNode source, report;

  std::cout << "Reading from file: " << filename << std::endl;
  galois::graphs::readGraph(graph, filename); 
  std::cout << "Read " << graph.size() << " nodes, "
    << graph.sizeEdges() << " edges" << std::endl;

  if (startNode >= graph.size() || reportNode >= graph.size()) {
    std::cerr << "failed to set report: " << reportNode
              << " or failed to set source: " << startNode << "\n";
    assert(0);
    abort();
  }

  auto it = graph.begin();
  std::advance(it, startNode);
  source = *it;
  it = graph.begin();
  std::advance(it, reportNode);
  report = *it;

  size_t approxNodeData = 4 * (graph.size() + graph.sizeEdges());
  // size_t approxEdgeData = graph.sizeEdges() * sizeof(typename
  // Graph::edge_data_type) * 2;
  galois::preAlloc(8*numThreads +
                   approxNodeData / galois::runtime::pagePoolSize());

  galois::reportPageAlloc("MeminfoPre");

  galois::do_all(galois::iterate(graph), 
                       [&graph] (GNode n) { graph.getData(n) = BFS::DIST_INFINITY; });
  graph.getData(source) = 0;

  galois::StatTimer Tmain;
  Tmain.start();

  switch(algo) {
    case Sync2p:
      std::cout << "Running Sync2p algorithm\n";
      sync2phaseAlgo(graph, source);
      break;
    case Sync:
      std::cout << "Running Sync algorithm\n";
      syncAlgo(graph, source);
      break;
    case Async:
      std::cout << "Running Async algorithm\n";
      asyncAlgo(graph, source);
      break;
    case Serial:
      std::cout << "Running Serial algorithm\n";
      serialAlgo(graph, source);
      break;
    case SerialSync:
      std::cout << "Running Serial 2 WL algorithm\n";
      serialSyncAlgo(graph, source);
      break;
    default:
      std::abort();
  }

  Tmain.stop();
  T.stop();

  galois::reportPageAlloc("MeminfoPost");
  galois::runtime::reportNumaAlloc("NumaPost");
  
  std::cout << "Node " << reportNode << " has distance "
            << graph.getData(report) << "\n";

  if (!skipVerify) {
    if (BFS::verify<true>(graph, source)) {
      std::cout << "Verification successful.\n";
    } else {
      GALOIS_DIE("Verification failed");
    }
  }


  return 0;
}
