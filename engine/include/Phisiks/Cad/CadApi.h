#pragma once
// PhisiksCad is a shared library (the geometry kernel, Open CASCADE, stays private to it).
#if defined(_WIN32)
#  if defined(PHISIKS_CAD_BUILD)
#    define PHISIKS_CAD_API __declspec(dllexport)
#  else
#    define PHISIKS_CAD_API __declspec(dllimport)
#  endif
#else
#  define PHISIKS_CAD_API __attribute__((visibility("default")))
#endif

#if defined(_MSC_VER)
#  pragma warning(disable : 4251)   // standard-library members of exported classes (same toolchain required, see SDK.md)
#endif

#include <cstdint>

namespace Phisiks::Cad {
    using Id = uint32_t;            // sketch geometry / constraint / feature id; 0 = none

    // Lengths in a document are in its unit; scenes are in metres.
    enum class Unit { Millimetre, Centimetre, Metre, Inch };
    PHISIKS_CAD_API double MetresPerUnit(Unit unit);
    PHISIKS_CAD_API const char* UnitSymbol(Unit unit);      // "mm", "cm", "m", "in"
    PHISIKS_CAD_API bool ParseUnit(const char* text, Unit& out);
}
