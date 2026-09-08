#pragma once
// Serial C-regular random graph generator for the neighborhood seed balancer.
//
// Trimmed from the standalone regular_graph.hh (configuration/pairing model
// + local switch repair). Deterministic for a given (N, C, seed): every PE
// generates the identical graph and reads its own adjacency row, so no
// broadcast is needed at startup. Spectral quality (lambda2 near the
// Ramanujan bound 2*sqrt(C-1)) was verified offline for N up to 32768,
// C in {4,6,8}.
//
// No Converse/Charm dependencies on purpose.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

class RegularGraph {
public:
  RegularGraph() = default;
  RegularGraph(int N, int C, uint64_t seed) { generate(N, C, seed); }

  int N() const { return N_; }
  int C() const { return C_; }
  const std::vector<int> &neighbors(int v) const { return g_[v]; }

  // (Re)generate a fresh graph. C must be even and N > C.
  void generate(int N, int C, uint64_t seed) {
    N_ = N;
    C_ = C;
    std::mt19937_64 rng(seed);
    const int MAX_ATTEMPTS = 200;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
      g_.assign(N, {});
      for (auto &row : g_)
        row.reserve(C);

      // Stub (half-edge) list: each vertex appears C times.
      std::vector<int> stubs;
      stubs.reserve(static_cast<size_t>(N) * C);
      for (int v = 0; v < N; ++v)
        for (int k = 0; k < C; ++k)
          stubs.push_back(v);

      // Fisher-Yates shuffle, then pair consecutive stubs.
      for (size_t i = stubs.size(); i > 1; --i) {
        size_t j = rng() % i;
        std::swap(stubs[i - 1], stubs[j]);
      }
      for (size_t i = 0; i + 1 < stubs.size(); i += 2) {
        int u = stubs[i], v = stubs[i + 1];
        g_[u].push_back(v);
        g_[v].push_back(u);
      }
      // g_ is now a C-regular multigraph; repair to a simple graph.
      if (repair(rng))
        return;
    }
    // In practice unreachable for C in [4,8]; leaves last attempt in place.
  }

  // Structural check: degree, symmetry, no self-loop, no parallel edge.
  bool verify() const {
    for (int v = 0; v < N_; ++v) {
      if (static_cast<int>(g_[v].size()) != C_)
        return false;
      for (int u : g_[v]) {
        if (u == v || u < 0 || u >= N_)
          return false;
        if (std::count(g_[u].begin(), g_[u].end(), v) !=
            std::count(g_[v].begin(), g_[v].end(), u))
          return false;
      }
      std::vector<int> s = g_[v];
      std::sort(s.begin(), s.end());
      if (std::adjacent_find(s.begin(), s.end()) != s.end())
        return false;
    }
    return true;
  }

private:
  int N_ = 0, C_ = 0;
  std::vector<std::vector<int>> g_;

  bool hasEdge(int u, int v) const {
    return std::find(g_[u].begin(), g_[u].end(), v) != g_[u].end();
  }
  void addEdge(int u, int v) {
    g_[u].push_back(v);
    g_[v].push_back(u);
  }
  void removeEdge(int u, int v) {
    g_[u].erase(std::find(g_[u].begin(), g_[u].end(), v));
    g_[v].erase(std::find(g_[v].begin(), g_[v].end(), u));
  }
  // Index of a bad neighbor in g_[u] (self-loop or duplicate), else -1.
  int badSlot(int u) const {
    const auto &nb = g_[u];
    for (size_t k = 0; k < nb.size(); ++k) {
      if (nb[k] == u)
        return static_cast<int>(k);
      for (size_t j = k + 1; j < nb.size(); ++j)
        if (nb[j] == nb[k])
          return static_cast<int>(k);
    }
    return -1;
  }

  bool repair(std::mt19937_64 &rng) {
    // Worklist of possibly-bad vertices; a switch only re-checks the few
    // vertices it touches: O(N) setup + O(defects) work.
    std::deque<int> work;
    std::vector<char> queued(N_, 0);
    auto enqueue = [&](int v) {
      if (!queued[v]) {
        queued[v] = 1;
        work.push_back(v);
      }
    };

    for (int v = 0; v < N_; ++v)
      if (badSlot(v) >= 0)
        enqueue(v);

    auto pickRandomHalfEdge = [&](int &x, int &y) {
      uint64_t idx = rng() % (static_cast<uint64_t>(N_) * C_);
      x = static_cast<int>(idx / C_);
      y = g_[x][idx % C_];
    };

    const long long cap = 200LL * static_cast<long long>(N_) * C_ + 100000;
    long long steps = 0;

    while (!work.empty()) {
      if (++steps > cap)
        return false; // give up; caller reshuffles
      int u = work.front();
      work.pop_front();
      queued[u] = 0;

      int bk = badSlot(u);
      if (bk < 0)
        continue;
      int vbad = g_[u][bk]; // problematic neighbor (== u if self-loop)

      int x, y;
      pickRandomHalfEdge(x, y);
      if (x == u || x == vbad || y == u || y == vbad) {
        enqueue(u);
        continue;
      }

      if (u == vbad) {
        // Self-loop at u: split it across edge (x,y) -> (u,x) and (u,y).
        if (hasEdge(u, x) || hasEdge(u, y)) {
          enqueue(u);
          continue;
        }
        removeEdge(u, u);
        removeEdge(x, y);
        addEdge(u, x);
        addEdge(u, y);
      } else {
        // Parallel edge (u,vbad): switch with (x,y) -> (u,x) and (vbad,y).
        if (hasEdge(u, x) || hasEdge(vbad, y)) {
          enqueue(u);
          continue;
        }
        removeEdge(u, vbad);
        removeEdge(x, y);
        addEdge(u, x);
        addEdge(vbad, y);
      }
      for (int w : {u, vbad, x, y})
        if (badSlot(w) >= 0)
          enqueue(w);
    }
    return true;
  }
};
