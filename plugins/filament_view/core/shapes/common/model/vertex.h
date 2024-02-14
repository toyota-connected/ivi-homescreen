
#include <filament/Box.h>

namespace plugin_filament_view {

class Color;

using Size = ::filament::math::float3;
using UVCoordinates = ::filament::math::float2;
using Transform = ::filament::math::mat4;

class Vertex {
  Vertex(::filament::math::float3 position,
         ::filament::math::float3 normal,
         UVCoordinates uvCoordinates,
         Color color);

  //  class Submesh(std::vector<int> triangleIndices) {
  //    constructor(vararg triangleIndices: Int) :
  //    this(triangleIndices.toList())
  //  }

  // fun FloatArray.toFloat3() = this.let { (x, y, z) -> Float3(x, y, z) }
  ::filament::math::float3 toFloat3(float x, float y, float z) {
    return ::filament::math::float3(x, y, z);
  }

  //  Box(center.toFloatArray(), halfExtent.toFloatArray())
  ::filament::Box Box(::filament::math::float3 center, ::filament::math::float3 halfExtent) {
    return ::filament::Box(center, halfExtent);
  }

  var Box.centerPosition : Position get() =
      center
          .toPosition() set(value){setCenter(value.x, value.y, value.z)}

      var Box.halfExtentSize : Size get() =
          halfExtent
              .toSize() set(value){setHalfExtent(value.x, value.y, value.z)}

          var Box.

          size
          get() = halfExtentSize *
                  2.0f

                  set(value){halfExtentSize = value / 2.0f}

                  fun FloatArray.

                  toPosition() = this.let{(x, y, z) -> Position(x, y, z)}

                                 fun FloatArray.

                                 toSize() = this.let{(x, y, z)->Size(x, y, z)}

                                            fun Mat4.

                                            toDoubleArray()
      : DoubleArray

      =

          toFloatArray()

              .map{it.toDouble()}
              .

          toDoubleArray()

              val Mat4.quaternion : Quaternion get() =
              rotation(this).toQuaternion()
};