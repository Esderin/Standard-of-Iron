#pragma once

#include "../core/entity.h"
#include "../core/system.h"

namespace Game::Systems {

class CleanupSystem : public Engine::Core::System {
public:
  void update(Engine::Core::World *world, float deltaTime) override;

private:
  void removeDeadEntities(Engine::Core::World *world);
};

} // namespace Game::Systems
