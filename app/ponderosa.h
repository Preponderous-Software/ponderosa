// Ponderosa on streetkit: the game's own names (ponderosa::Settings and the rest of settings.h) live beside the
// library's, so ponderosa::Walker, ponderosa::Overview and friends are streetkit's.
#pragma once

namespace streetkit {}
namespace ponderosa {
using namespace streetkit;
}
