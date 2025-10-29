#include "pathfinding.h"
#include "../map/terrain_service.h"
#include "building_collision_registry.h"
#include "map/terrain.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace Game::Systems {

Pathfinding::Pathfinding(int width, int height)
    : m_width(width), m_height(height) {
  m_obstacles.resize(height, std::vector<std::uint8_t>(width, 0));
  ensureWorkingBuffers();
  m_obstaclesDirty.store(true, std::memory_order_release);
  m_workerThread = std::thread(&Pathfinding::workerLoop, this);
}

Pathfinding::~Pathfinding() {
  m_stopWorker.store(true, std::memory_order_release);
  m_requestCondition.notify_all();
  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }
}

void Pathfinding::setGridOffset(float offset_x, float offset_z) {
  m_gridOffsetX = offset_x;
  m_gridOffsetZ = offset_z;
}

void Pathfinding::setObstacle(int x, int y, bool isObstacle) {
  if (x >= 0 && x < m_width && y >= 0 && y < m_height) {
    m_obstacles[y][x] = static_cast<std::uint8_t>(isObstacle);
  }
}

auto Pathfinding::isWalkable(int x, int y) const -> bool {
  if (x < 0 || x >= m_width || y < 0 || y >= m_height) {
    return false;
  }
  return m_obstacles[y][x] == 0;
}

void Pathfinding::markObstaclesDirty() {
  m_obstaclesDirty.store(true, std::memory_order_release);
}

void Pathfinding::updateBuildingObstacles() {

  if (!m_obstaclesDirty.load(std::memory_order_acquire)) {
    return;
  }

  std::lock_guard<std::mutex> const lock(m_mutex);

  if (!m_obstaclesDirty.load(std::memory_order_acquire)) {
    return;
  }

  for (auto &row : m_obstacles) {
    std::fill(row.begin(), row.end(), static_cast<std::uint8_t>(0));
  }

  auto &terrain_service = Game::Map::TerrainService::instance();
  if (terrain_service.isInitialized()) {
    const Game::Map::TerrainHeightMap *height_map =
        terrain_service.getHeightMap();
    const int terrain_width =
        (height_map != nullptr) ? height_map->getWidth() : 0;
    const int terrain_height =
        (height_map != nullptr) ? height_map->getHeight() : 0;

    for (int z = 0; z < m_height; ++z) {
      for (int x = 0; x < m_width; ++x) {
        bool blocked = false;
        if (x < terrain_width && z < terrain_height) {
          blocked = !terrain_service.isWalkable(x, z);
        } else {
          blocked = true;
        }

        if (blocked) {
          m_obstacles[z][x] = static_cast<std::uint8_t>(1);
        }
      }
    }
  }

  auto &registry = BuildingCollisionRegistry::instance();
  const auto &buildings = registry.getAllBuildings();

  for (const auto &building : buildings) {
    auto cells = Game::Systems::BuildingCollisionRegistry::getOccupiedGridCells(
        building, m_gridCellSize);
    for (const auto &cell : cells) {
      int const grid_x =
          static_cast<int>(std::round(cell.first - m_gridOffsetX));
      int const grid_z =
          static_cast<int>(std::round(cell.second - m_gridOffsetZ));

      if (grid_x >= 0 && grid_x < m_width && grid_z >= 0 && grid_z < m_height) {
        m_obstacles[grid_z][grid_x] = static_cast<std::uint8_t>(1);
      }
    }
  }

  m_obstaclesDirty.store(false, std::memory_order_release);
}

auto Pathfinding::findPath(const Point &start,
                           const Point &end) -> std::vector<Point> {

  if (m_obstaclesDirty.load(std::memory_order_acquire)) {
    updateBuildingObstacles();
  }

  std::lock_guard<std::mutex> const lock(m_mutex);
  return findPathInternal(start, end);
}

auto Pathfinding::findPathAsync(const Point &start, const Point &end)
    -> std::future<std::vector<Point>> {
  return std::async(std::launch::async,
                    [this, start, end]() { return findPath(start, end); });
}

void Pathfinding::submitPathRequest(std::uint64_t request_id,
                                    const Point &start, const Point &end) {
  {
    std::lock_guard<std::mutex> const lock(m_requestMutex);
    m_requestQueue.push({request_id, start, end});
  }
  m_requestCondition.notify_one();
}

auto Pathfinding::fetchCompletedPaths()
    -> std::vector<Pathfinding::PathResult> {
  std::vector<PathResult> results;
  std::lock_guard<std::mutex> const lock(m_resultMutex);
  while (!m_resultQueue.empty()) {
    results.push_back(std::move(m_resultQueue.front()));
    m_resultQueue.pop();
  }
  return results;
}

auto Pathfinding::findPathInternal(const Point &start,
                                   const Point &end) -> std::vector<Point> {
  ensureWorkingBuffers();

  if (!isWalkable(start.x, start.y) || !isWalkable(end.x, end.y)) {
    return {};
  }

  const int start_idx = toIndex(start);
  const int end_idx = toIndex(end);

  if (start_idx == end_idx) {
    return {start};
  }

  const std::uint32_t generation = nextGeneration();

  m_openHeap.clear();

  setGCost(start_idx, generation, 0);
  setParent(start_idx, generation, start_idx);

  pushOpenNode({start_idx, calculateHeuristic(start, end), 0});

  const int max_iterations = std::max(m_width * m_height, 1);
  int iterations = 0;

  int final_cost = -1;

  while (!m_openHeap.empty() && iterations < max_iterations) {
    ++iterations;

    QueueNode const current = popOpenNode();

    if (current.gCost > getGCost(current.index, generation)) {
      continue;
    }

    if (isClosed(current.index, generation)) {
      continue;
    }

    setClosed(current.index, generation);

    if (current.index == end_idx) {
      final_cost = current.gCost;
      break;
    }

    const Point current_point = toPoint(current.index);
    std::array<Point, 8> neighbors{};
    const std::size_t neighbor_count =
        collectNeighbors(current_point, neighbors);

    for (std::size_t i = 0; i < neighbor_count; ++i) {
      const Point &neighbor = neighbors[i];
      if (!isWalkable(neighbor.x, neighbor.y)) {
        continue;
      }

      const int neighbor_idx = toIndex(neighbor);
      if (isClosed(neighbor_idx, generation)) {
        continue;
      }

      const int tentative_gcost = current.gCost + 1;
      if (tentative_gcost >= getGCost(neighbor_idx, generation)) {
        continue;
      }

      setGCost(neighbor_idx, generation, tentative_gcost);
      setParent(neighbor_idx, generation, current.index);

      const int h_cost = calculateHeuristic(neighbor, end);
      pushOpenNode({neighbor_idx, tentative_gcost + h_cost, tentative_gcost});
    }
  }

  if (final_cost < 0) {
    return {};
  }

  std::vector<Point> path;
  path.reserve(final_cost + 1);
  buildPath(start_idx, end_idx, generation, final_cost + 1, path);
  return path;
}

auto Pathfinding::calculateHeuristic(const Point &a, const Point &b) -> int {
  return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

void Pathfinding::ensureWorkingBuffers() {
  const std::size_t total_cells =
      static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height);

  if (m_closedGeneration.size() != total_cells) {
    m_closedGeneration.assign(total_cells, 0);
    m_gCostGeneration.assign(total_cells, 0);
    m_gCostValues.assign(total_cells, std::numeric_limits<int>::max());
    m_parentGeneration.assign(total_cells, 0);
    m_parentValues.assign(total_cells, -1);
  }

  const std::size_t min_open_capacity =
      std::max<std::size_t>(total_cells / 8, 64);
  if (m_openHeap.capacity() < min_open_capacity) {
    m_openHeap.reserve(min_open_capacity);
  }
}

auto Pathfinding::nextGeneration() -> std::uint32_t {
  auto next = ++m_generationCounter;
  if (next == 0) {
    resetGenerations();
    next = ++m_generationCounter;
  }
  return next;
}

void Pathfinding::resetGenerations() {
  std::fill(m_closedGeneration.begin(), m_closedGeneration.end(), 0);
  std::fill(m_gCostGeneration.begin(), m_gCostGeneration.end(), 0);
  std::fill(m_parentGeneration.begin(), m_parentGeneration.end(), 0);
  std::fill(m_gCostValues.begin(), m_gCostValues.end(),
            std::numeric_limits<int>::max());
  std::fill(m_parentValues.begin(), m_parentValues.end(), -1);
  m_generationCounter = 0;
}

auto Pathfinding::isClosed(int index, std::uint32_t generation) const -> bool {
  return index >= 0 &&
         static_cast<std::size_t>(index) < m_closedGeneration.size() &&
         m_closedGeneration[static_cast<std::size_t>(index)] == generation;
}

void Pathfinding::setClosed(int index, std::uint32_t generation) {
  if (index >= 0 &&
      static_cast<std::size_t>(index) < m_closedGeneration.size()) {
    m_closedGeneration[static_cast<std::size_t>(index)] = generation;
  }
}

auto Pathfinding::getGCost(int index, std::uint32_t generation) const -> int {
  if (index < 0 ||
      static_cast<std::size_t>(index) >= m_gCostGeneration.size()) {
    return std::numeric_limits<int>::max();
  }
  if (m_gCostGeneration[static_cast<std::size_t>(index)] == generation) {
    return m_gCostValues[static_cast<std::size_t>(index)];
  }
  return std::numeric_limits<int>::max();
}

void Pathfinding::setGCost(int index, std::uint32_t generation, int cost) {
  if (index >= 0 &&
      static_cast<std::size_t>(index) < m_gCostGeneration.size()) {
    const auto idx = static_cast<std::size_t>(index);
    m_gCostGeneration[idx] = generation;
    m_gCostValues[idx] = cost;
  }
}

auto Pathfinding::hasParent(int index, std::uint32_t generation) const -> bool {
  return index >= 0 &&
         static_cast<std::size_t>(index) < m_parentGeneration.size() &&
         m_parentGeneration[static_cast<std::size_t>(index)] == generation;
}

auto Pathfinding::getParent(int index, std::uint32_t generation) const -> int {
  if (hasParent(index, generation)) {
    return m_parentValues[static_cast<std::size_t>(index)];
  }
  return -1;
}

void Pathfinding::setParent(int index, std::uint32_t generation,
                            int parentIndex) {
  if (index >= 0 &&
      static_cast<std::size_t>(index) < m_parentGeneration.size()) {
    const auto idx = static_cast<std::size_t>(index);
    m_parentGeneration[idx] = generation;
    m_parentValues[idx] = parentIndex;
  }
}

auto Pathfinding::collectNeighbors(
    const Point &point, std::array<Point, 8> &buffer) const -> std::size_t {
  std::size_t count = 0;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) {
      if (dx == 0 && dy == 0) {
        continue;
      }

      const int x = point.x + dx;
      const int y = point.y + dy;

      if (x < 0 || x >= m_width || y < 0 || y >= m_height) {
        continue;
      }

      if (dx != 0 && dy != 0) {
        if (!isWalkable(point.x + dx, point.y) ||
            !isWalkable(point.x, point.y + dy)) {
          continue;
        }
      }

      buffer[count++] = Point{x, y};
    }
  }
  return count;
}

void Pathfinding::buildPath(int startIndex, int endIndex,
                            std::uint32_t generation, int expectedLength,
                            std::vector<Point> &outPath) const {
  outPath.clear();
  if (expectedLength > 0) {
    outPath.reserve(static_cast<std::size_t>(expectedLength));
  }
  int current = endIndex;

  while (current >= 0) {
    outPath.push_back(toPoint(current));
    if (current == startIndex) {
      std::reverse(outPath.begin(), outPath.end());
      return;
    }

    if (!hasParent(current, generation)) {
      outPath.clear();
      return;
    }

    const int parent = getParent(current, generation);
    if (parent == current || parent < 0) {
      outPath.clear();
      return;
    }
    current = parent;
  }

  outPath.clear();
}

auto Pathfinding::heapLess(const QueueNode &lhs, const QueueNode &rhs) -> bool {
  if (lhs.fCost != rhs.fCost) {
    return lhs.fCost < rhs.fCost;
  }
  return lhs.gCost < rhs.gCost;
}

void Pathfinding::pushOpenNode(const QueueNode &node) {
  m_openHeap.push_back(node);
  std::size_t index = m_openHeap.size() - 1;
  while (index > 0) {
    std::size_t const parent = (index - 1) / 2;
    if (heapLess(m_openHeap[parent], m_openHeap[index])) {
      break;
    }
    std::swap(m_openHeap[parent], m_openHeap[index]);
    index = parent;
  }
}

auto Pathfinding::popOpenNode() -> Pathfinding::QueueNode {
  QueueNode top = m_openHeap.front();
  QueueNode const last = m_openHeap.back();
  m_openHeap.pop_back();
  if (!m_openHeap.empty()) {
    m_openHeap[0] = last;
    std::size_t index = 0;
    const std::size_t size = m_openHeap.size();
    while (true) {
      std::size_t const left = index * 2 + 1;
      std::size_t const right = left + 1;
      std::size_t smallest = index;

      if (left < size && !heapLess(m_openHeap[smallest], m_openHeap[left])) {
        smallest = left;
      }
      if (right < size && !heapLess(m_openHeap[smallest], m_openHeap[right])) {
        smallest = right;
      }
      if (smallest == index) {
        break;
      }
      std::swap(m_openHeap[index], m_openHeap[smallest]);
      index = smallest;
    }
  }
  return top;
}

void Pathfinding::workerLoop() {
  while (true) {
    PathRequest request;
    {
      std::unique_lock<std::mutex> lock(m_requestMutex);
      m_requestCondition.wait(lock, [this]() {
        return m_stopWorker.load(std::memory_order_acquire) ||
               !m_requestQueue.empty();
      });

      if (m_stopWorker.load(std::memory_order_acquire) &&
          m_requestQueue.empty()) {
        break;
      }

      request = m_requestQueue.front();
      m_requestQueue.pop();
    }

    auto path = findPath(request.start, request.end);

    {
      std::lock_guard<std::mutex> const lock(m_resultMutex);
      m_resultQueue.push({request.request_id, std::move(path)});
    }
  }
}

} // namespace Game::Systems