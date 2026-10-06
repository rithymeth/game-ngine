#include "aether/gameplay/gameplay_kit.h"

#include "aether/gameplay/attribute_system.h"

namespace aether::gas {

namespace {

class GameplayKit : public kit::IKit {
public:
    const char* Name() const override { return "Gameplay"; }
    void RegisterComponents() override { RegisterGameplayComponents(); }
};

} // namespace

std::unique_ptr<kit::IKit> MakeGameplayKit() { return std::make_unique<GameplayKit>(); }

} // namespace aether::gas
