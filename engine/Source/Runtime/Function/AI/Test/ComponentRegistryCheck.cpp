// Verifies that the components used by the legacy .object.json definitions are
// reachable through the runtime type system. Compiling is not enough: a class
// only becomes loadable once IMPLEMENT_REGISTER_CLASS has run and registered
// it under its type name, and .object.json refers to components by name
// ("RigidBodyComponent", "AnimationComponent", ...). If a name does not
// resolve, prefab instantiation will silently drop that component.

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"

#include <cstdio>

namespace
{
    int g_failures = 0;

    void ExpectResolvable(const char* type_name)
    {
        const Type* type = TypeManager::GetInstance().ClassNameToType(type_name);
        if (type == nullptr)
        {
            std::fprintf(stderr, "  FAIL  %-24s not registered\n", type_name);
            ++g_failures;
            return;
        }

        Object* produced = GET_SYSTEM(ObjectManager)->Produce(type, 0);
        if (produced == nullptr)
        {
            std::fprintf(stderr, "  FAIL  %-24s registered but not producible\n", type_name);
            ++g_failures;
            return;
        }

        std::fprintf(stderr, "  ok    %-24s registered + producible\n", type_name);
    }
}  // namespace

int main()
{
    RegisterRuntime();
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);

    std::fprintf(stderr, "components referenced by .object.json definitions:\n");
    ExpectResolvable("Transform");
    ExpectResolvable("TransformComponent");  // legacy alias
    ExpectResolvable("MeshRenderer");
    ExpectResolvable("SkinMeshRenderer");
    ExpectResolvable("RigidBodyComponent");
    ExpectResolvable("AnimationComponent");
    ExpectResolvable("MotorComponent");
    ExpectResolvable("ParticleComponent");
    ExpectResolvable("CameraComponent");
    ExpectResolvable("NavMeshVolumeComponent");

    std::fprintf(stderr, "\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
