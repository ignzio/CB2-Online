#pragma once
// Scenery (type 3) authoring: re-encode a Scenery into an entry, and a Wavefront OBJ bridge so that levels can be
// edited in any 3D package (Blender etc.) and brought back.
//
// OBJ conventions (world units are divided by 400 like the viewer):
//   v x y z r g b        vertex position and colour (0..1, 1.0 = 128 on the PS1; values above 1 are allowed)
//   usemtl t<N> | a<N>   following faces use texture description N (1-based) / animated texture N; t0 = untextured
//   f a b c  /  f a b c d     triangle / quad (quads are drawn as a-b-c and a-d-c); only the vertex index is used
// Texture coordinates are not part of the OBJ: they live in the shared texture descriptions, so faces keep the
// look of the descriptor they reference. Creating new descriptors and texture pages is not supported yet.
#include <string>

#include "scenery.hpp"

namespace cb2 {

// Items 0-3 and 5 are rewritten from `s`; the other items (texture descriptions, animation table) are kept
// from `original`. Throws std::runtime_error when a value does not fit the format (4096 vertices, 1024 colours,
// 12-bit coordinates, 2047 textures).
Entry packScenery(const Scenery& s, const Entry& original);

std::string sceneryToObj(const Scenery& s);

// Applies an edited OBJ to `base`: positions, colours and faces are replaced; textures and offsets are kept.
// Throws std::runtime_error with the offending line on malformed input.
Scenery sceneryFromObj(const std::string& obj, const Scenery& base);

}  // namespace cb2
