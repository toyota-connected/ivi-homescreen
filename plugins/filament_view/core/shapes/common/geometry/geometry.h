
#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/VertexBuffer.h>
#include <utils/EntityManager.h>

namespace plugin_filament_view {

/**
 * Geometry parameters for building and updating a Renderable
 *
 * A renderable is made of several primitives.
 * You can ever declare only 1 if you want each parts of your Geometry to have
 * the same material or one for each triangle indices with a different material.
 * We could declare n primitives (n per face) and give each of them a different
 * material instance, setup with different parameters
 *
 */
class Geometry {
 public:
  Geometry(::filament::VertexBuffer vertexBuffer,
           ::filament::IndexBuffer indexBuffer,
           std::optional<::filament::Box> boundingBox,
           std::vector<std::pair<int, int>> offsetsCounts,
           std::vector<Vertex> vertics,
           std::vector<Submesh> submeshes);
  ~Geometry();

  auto renderable = engine->getEntityManager().create();
}

private : static constexpr size_t kPositionSize = 3;  // x, y, z
static constexpr size_t kTangentSize = 4;             // Quaternion: x, y, z, w
static constexpr size_t kUVSize = 2;                  // x, y
static constexpr size_t kColorSize = 4;               // r, g, b, a
};
}  // namespace plugin_filament_view