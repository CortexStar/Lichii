// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/exception.h"
#include "shader_recompiler/stage.h"

namespace Shader {

inline u32 NvnStorageBufferBase(Stage stage) {
    switch (stage) {
    case Stage::VertexA:
    case Stage::VertexB:
        return 0x110;
    case Stage::TessellationControl:
        return 0x210;
    case Stage::TessellationEval:
    case Stage::Compute:
        return 0x310;
    case Stage::Geometry:
        return 0x410;
    case Stage::Fragment:
        return 0x510;
    case Stage::Task:
        return 0x670;
    case Stage::Mesh:
        return 0x770;
    }
    throw InvalidArgument("Invalid stage {}", stage);
}

} // namespace Shader
