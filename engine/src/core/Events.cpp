#include "Phisiks/Core/Events.h"

namespace Phisiks {
    EventBus& GlobalEvents() {
        static EventBus bus;
        return bus;
    }
}
