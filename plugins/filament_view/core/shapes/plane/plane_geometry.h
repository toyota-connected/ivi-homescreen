
#include <filament/Engine.h>

#include "core/shapes/common/geometry/geometry.h"
#include "models/scene/geometry/direction.h"
#include "models/scene/geometry/position.h"
#include "models/scene/geometry/size.h"

class Direction;

class Geometry;

class Normal;

class Size;

namespace plugin_filament_view {

class PlaneGeometry : public Geometry {
 public:
  PlaneGeometry(::filament::float3 center, Size size, Direction normal, Geometry geometry)
      : Geometry(geometry.vertexBuffer,
                 geometry.indexBuffer,
                 geometry.boundingBox,
                 geometry.offsetsCounts,
                 geometry.vertices,
                 geometry.submeshes){

        };

  /**
   * Creates a [Geometry] in the shape of a plane with the give specifications
   *
   * @param center Center of the constructed plane
   * @param size  Size of the constructed plane
   */
  class Builder(::filament::float3 center = ::filament::float3(0.0f, 0f, -4f),
                ::filament::float3 size = ::filament::float3(2.0f, 2.0f),
                ::filament::float3 normal = ::filament::float3(0f, 0f, 1f))
      : Geometry.Builder {
    void build(::filament::Engine * Engine)
        : PlaneGeometry =
              PlaneGeometry(center, size, normal, super.build(engine)) override;
  }

  void update(::filament::Engine* engine,
              ::filament::math::float3 normal,
              ::filament::math::float3 center(this->center),
              Size

                  size = (this.size)){
      setBufferVertices(engine, getVertices(center, size, normal))}

  std::vector<Vertex> getVertices(Position center = Position(0.0f),
                                  Size size = Size(2.0f, 2.0f),
                                  Direction normal = Direction(0f, 0f, 1f)) {
    auto extents = size / 2.0f;

    auto p0 = center + Size(-extents.x, -extents.y, extents.z);
    auto p1 = center + Size(-extents.x, extents.y, -extents.z);
    auto p2 = center + Size(extents.x, extents.y, -extents.z);
    auto p3 = center + Size(extents.x, -extents.y, extents.z);

    auto uv00 = UVCoordinates(0.0f, 0.0f);
    auto uv10 = UVCoordinates(1.0f, 0.0f);
    auto uv01 = UVCoordinates(0.0f, 1.0f);
    auto uv11 = UVCoordinates(1.0f, 1.0f);

    add(Vertex(position = p0, normal = normal, uvCoordinates = uv00));
    add(Vertex(position = p1, normal = normal, uvCoordinates = uv01));
    add(Vertex(position = p2, normal = normal, uvCoordinates = uv11));
    add(Vertex(position = p3, normal = normal, uvCoordinates = uv10));
  }

 private:
  /**
   * Center of the constructed plane
   */
  Position center_;

  /**
   * Size of the constructed plane
   */
  Size size_;

  Direction normal_;
};
}  // namespace plugin_filament_view