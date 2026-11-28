#pragma once
// nlohmann/json (MIT, vendor/nlohmann), private to PhisiksCad.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace Phisiks::Cad {
    using json = nlohmann::json;
}
