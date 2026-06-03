#ifndef NOTCH_MOCK_VERIFY_STRUCTURAL_H
#define NOTCH_MOCK_VERIFY_STRUCTURAL_H

#include <cstdint>
#include <vector>

#include "matrix.h"  // VerifyResult

namespace notch_mock
{
// Structural validity of a reserialized payload (file_3d / mesh GLB,
// load3d_camera JSON). save_to / save_glb_from_mesh / save_json reserialize, so
// the delivered bytes legitimately differ from the upload — verify the container
// is valid and coherent, not an exact source hash. Auto-detects GLB (magic) vs
// JSON; a light, dependency-free check (no full parser).
VerifyResult VerifyStructural(const std::vector<uint8_t>& delivered);
}

#endif
