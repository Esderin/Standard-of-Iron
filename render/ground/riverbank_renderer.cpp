#include "riverbank_renderer.h"
#include "../../game/map/visibility_service.h"
#include "../gl/mesh.h"
#include "../gl/resources.h"
#include "../scene_renderer.h"
#include "ground_utils.h"
#include "map/terrain.h"
#include <QVector2D>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <qglobal.h>
#include <qmatrix4x4.h>
#include <qvectornd.h>
#include <utility>
#include <vector>

namespace Render::GL {

RiverbankRenderer::RiverbankRenderer() = default;
RiverbankRenderer::~RiverbankRenderer() = default;

void RiverbankRenderer::configure(
    const std::vector<Game::Map::RiverSegment> &riverSegments,
    const Game::Map::TerrainHeightMap &height_map) {
  m_riverSegments = riverSegments;
  m_tile_size = height_map.getTileSize();
  m_grid_width = height_map.getWidth();
  m_grid_height = height_map.getHeight();
  m_heights = height_map.getHeightData();
  buildMeshes();
}

void RiverbankRenderer::buildMeshes() {
  m_meshes.clear();
  m_visibilitySamples.clear();

  if (m_riverSegments.empty()) {
    return;
  }

  auto noise = [](float x, float y) -> float {
    float const ix = std::floor(x);
    float const iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    fx = fx * fx * (3.0F - 2.0F * fx);
    fy = fy * fy * (3.0F - 2.0F * fy);

    float const a = Ground::noise_hash(ix, iy);
    float const b = Ground::noise_hash(ix + 1.0F, iy);
    float const c = Ground::noise_hash(ix, iy + 1.0F);
    float const d = Ground::noise_hash(ix + 1.0F, iy + 1.0F);

    return a * (1.0F - fx) * (1.0F - fy) + b * fx * (1.0F - fy) +
           c * (1.0F - fx) * fy + d * fx * fy;
  };

  auto sample_height = [&](float world_x, float world_z) -> float {
    if (m_heights.empty() || m_grid_width == 0 || m_grid_height == 0) {
      return 0.0F;
    }

    float const half_width = m_grid_width * 0.5F - 0.5F;
    float const half_height = m_grid_height * 0.5F - 0.5F;
    float gx = (world_x / m_tile_size) + half_width;
    float gz = (world_z / m_tile_size) + half_height;

    gx = std::clamp(gx, 0.0F, float(m_grid_width - 1));
    gz = std::clamp(gz, 0.0F, float(m_grid_height - 1));

    int const x0 = int(std::floor(gx));
    int const z0 = int(std::floor(gz));
    int const x1 = std::min(x0 + 1, m_grid_width - 1);
    int const z1 = std::min(z0 + 1, m_grid_height - 1);

    float const tx = gx - float(x0);
    float const tz = gz - float(z0);

    float const h00 = m_heights[z0 * m_grid_width + x0];
    float const h10 = m_heights[z0 * m_grid_width + x1];
    float const h01 = m_heights[z1 * m_grid_width + x0];
    float const h11 = m_heights[z1 * m_grid_width + x1];

    float const h0 = h00 * (1.0F - tx) + h10 * tx;
    float const h1 = h01 * (1.0F - tx) + h11 * tx;
    return h0 * (1.0F - tz) + h1 * tz;
  };

  for (const auto &segment : m_riverSegments) {
    QVector3D dir = segment.end - segment.start;
    float const length = dir.length();
    if (length < 0.01F) {
      m_meshes.push_back(nullptr);
      m_visibilitySamples.emplace_back();
      continue;
    }

    dir.normalize();
    QVector3D const perpendicular(-dir.z(), 0.0F, dir.x());
    float const half_width = segment.width * 0.5F;

    float const bank_width = 0.2F;

    int length_steps =
        static_cast<int>(std::ceil(length / (m_tile_size * 0.5F))) + 1;
    length_steps = std::max(length_steps, 8);

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<QVector3D> samples;

    for (int i = 0; i < length_steps; ++i) {
      float const t =
          static_cast<float>(i) / static_cast<float>(length_steps - 1);
      QVector3D center_pos = segment.start + dir * (length * t);

      constexpr float k_edge_noise_freq_1 = 2.0F;
      constexpr float k_edge_noise_freq_2 = 5.0F;
      constexpr float k_edge_noise_freq_3 = 10.0F;

      float const edge_noise1 =
          noise(center_pos.x() * k_edge_noise_freq_1, center_pos.z() * k_edge_noise_freq_1);
      float const edge_noise2 =
          noise(center_pos.x() * k_edge_noise_freq_2, center_pos.z() * k_edge_noise_freq_2);
      float const edge_noise3 =
          noise(center_pos.x() * k_edge_noise_freq_3, center_pos.z() * k_edge_noise_freq_3);

      float combined_noise =
          edge_noise1 * 0.5F + edge_noise2 * 0.3F + edge_noise3 * 0.2F;
      combined_noise = (combined_noise - 0.5F) * 2.0F;

      float const width_variation = combined_noise * half_width * 0.35F;

      float const meander = noise(t * 3.0F, length * 0.1F) * 0.3F;
      QVector3D const center_offset = perpendicular * meander;
      center_pos += center_offset;

      QVector3D const inner_left =
          center_pos - perpendicular * (half_width + width_variation);
      QVector3D const inner_right =
          center_pos + perpendicular * (half_width + width_variation);
      samples.push_back(inner_left);
      samples.push_back(inner_right);

      float const outer_variation =
          noise(center_pos.x() * 8.0F, center_pos.z() * 8.0F) * 0.5F;
      QVector3D const outer_left =
          inner_left - perpendicular * (bank_width + outer_variation);
      QVector3D const outer_right =
          inner_right + perpendicular * (bank_width + outer_variation);

      float const normal[3] = {0.0F, 1.0F, 0.0F};

      Vertex left_inner;
      Vertex left_outer;
      float const height_inner_left =
          sample_height(inner_left.x(), inner_left.z());
      float const height_outer_left =
          sample_height(outer_left.x(), outer_left.z());

      left_inner.position[0] = inner_left.x();
      left_inner.position[1] = height_inner_left + 0.05F;
      left_inner.position[2] = inner_left.z();
      left_inner.normal[0] = normal[0];
      left_inner.normal[1] = normal[1];
      left_inner.normal[2] = normal[2];
      left_inner.tex_coord[0] = 0.0F;
      left_inner.tex_coord[1] = t;
      vertices.push_back(left_inner);

      left_outer.position[0] = outer_left.x();
      left_outer.position[1] = height_outer_left + 0.05F;
      left_outer.position[2] = outer_left.z();
      left_outer.normal[0] = normal[0];
      left_outer.normal[1] = normal[1];
      left_outer.normal[2] = normal[2];
      left_outer.tex_coord[0] = 1.0F;
      left_outer.tex_coord[1] = t;
      vertices.push_back(left_outer);

      Vertex right_inner;
      Vertex right_outer;
      float const height_inner_right =
          sample_height(inner_right.x(), inner_right.z());
      float const height_outer_right =
          sample_height(outer_right.x(), outer_right.z());

      right_inner.position[0] = inner_right.x();
      right_inner.position[1] = height_inner_right + 0.05F;
      right_inner.position[2] = inner_right.z();
      right_inner.normal[0] = normal[0];
      right_inner.normal[1] = normal[1];
      right_inner.normal[2] = normal[2];
      right_inner.tex_coord[0] = 0.0F;
      right_inner.tex_coord[1] = t;
      vertices.push_back(right_inner);

      right_outer.position[0] = outer_right.x();
      right_outer.position[1] = height_outer_right + 0.05F;
      right_outer.position[2] = outer_right.z();
      right_outer.normal[0] = normal[0];
      right_outer.normal[1] = normal[1];
      right_outer.normal[2] = normal[2];
      right_outer.tex_coord[0] = 1.0F;
      right_outer.tex_coord[1] = t;
      vertices.push_back(right_outer);

      if (i < length_steps - 1) {
        unsigned int const idx0 = i * 4;

        indices.push_back(idx0 + 0);
        indices.push_back(idx0 + 4);
        indices.push_back(idx0 + 1);

        indices.push_back(idx0 + 1);
        indices.push_back(idx0 + 4);
        indices.push_back(idx0 + 5);

        indices.push_back(idx0 + 2);
        indices.push_back(idx0 + 3);
        indices.push_back(idx0 + 6);

        indices.push_back(idx0 + 3);
        indices.push_back(idx0 + 7);
        indices.push_back(idx0 + 6);
      }
    }

    if (!vertices.empty() && !indices.empty()) {
      m_meshes.push_back(std::make_unique<Mesh>(vertices, indices));
      m_visibilitySamples.push_back(std::move(samples));
    } else {
      m_meshes.push_back(nullptr);
      m_visibilitySamples.emplace_back();
    }
  }
}

void RiverbankRenderer::submit(Renderer &renderer, ResourceManager *resources) {
  if (m_meshes.empty() || m_riverSegments.empty()) {
    return;
  }

  Q_UNUSED(resources);

  auto &visibility = Game::Map::VisibilityService::instance();
  const bool use_visibility = visibility.isInitialized();

  auto *shader = renderer.getShader("riverbank");
  if (shader == nullptr) {
    return;
  }

  renderer.setCurrentShader(shader);

  QMatrix4x4 model;
  model.setToIdentity();

  size_t mesh_index = 0;
  for (const auto &segment : m_riverSegments) {
    if (mesh_index >= m_meshes.size()) {
      break;
    }

    auto *mesh = m_meshes[mesh_index].get();
    ++mesh_index;

    if (mesh == nullptr) {
      continue;
    }

    if (use_visibility) {
      bool any_visible = false;
      if (mesh_index - 1 < m_visibilitySamples.size()) {
        const auto &samples = m_visibilitySamples[mesh_index - 1];
        const int min_required =
            std::max<int>(2, static_cast<int>(samples.size() * 0.3F));
        int visible_count = 0;
        for (const auto &pos : samples) {
          if (visibility.isVisibleWorld(pos.x(), pos.z())) {
            ++visible_count;
            if (visible_count >= min_required) {
              any_visible = true;
              break;
            }
          }
        }
      }
      if (!any_visible) {
        continue;
      }
    }

    renderer.mesh(mesh, model, QVector3D(1.0F, 1.0F, 1.0F), nullptr, 1.0F);
  }

  renderer.setCurrentShader(nullptr);
}

} // namespace Render::GL
