#include "OldMcDonaldMaterials.h"
#include "ProjectKit.h"

namespace Projects::OldMcDonald {
    void RegisterHardwareMaterials() {
        auto M = [](const char* n, glm::vec3 c, float r, float m = 0.0f) { ProjectKit::Material(n, c, r, m); };
        M("OMD/Aluminium", { 0.86f, 0.87f, 0.88f }, 0.34f, 1.0f);
        M("OMD/Anodised", { 0.035f, 0.037f, 0.042f }, 0.42f, 0.6f);
        M("OMD/Stainless", { 0.66f, 0.67f, 0.68f }, 0.26f, 1.0f);
        M("OMD/MotorBlack", { 0.04f, 0.04f, 0.045f }, 0.30f, 0.8f);
        M("OMD/PCBDark", { 0.05f, 0.10f, 0.08f }, 0.40f);
        M("OMD/DarkPlastic", { 0.045f, 0.047f, 0.052f }, 0.55f);
        M("OMD/Wire/Red", { 0.62f, 0.04f, 0.03f }, 0.45f);
        M("OMD/Wire/Black", { 0.025f, 0.025f, 0.028f }, 0.45f);
        M("OMD/Wire/Yellow", { 0.80f, 0.62f, 0.05f }, 0.45f);
        M("OMD/Wire/Green", { 0.05f, 0.45f, 0.12f }, 0.45f);
        M("OMD/Wire/Blue", { 0.05f, 0.18f, 0.70f }, 0.45f);
        M("OMD/Wire/White", { 0.82f, 0.82f, 0.80f }, 0.45f);
        M("OMD/Cable", { 0.06f, 0.062f, 0.068f }, 0.60f);
    }
}
