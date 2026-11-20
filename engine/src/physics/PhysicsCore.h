#pragma once
// Engine-private: shared PhysX core (foundation + PxPhysics). PhysX allows a single foundation per
// process, so every PhysX user in the process (physics world, cooking, future GPU/XPBD coupling)
// borrows it from here instead of creating its own.

namespace physx { class PxFoundation; class PxPhysics; }

namespace Phisiks::Physics::Core {
    // Creates the foundation/physics on first use; reference counted.
    bool Acquire();
    void Release();
    physx::PxFoundation* Foundation();
    physx::PxPhysics* SDK();
}
