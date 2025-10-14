#pragma once

#include "ai_behavior_registry.h"
#include "ai_types.h"
#include <vector>

namespace Game::Systems::AI {

class AIExecutor {
public:
  AIExecutor() = default;
  ~AIExecutor() = default;

  AIExecutor(const AIExecutor &) = delete;
  AIExecutor &operator=(const AIExecutor &) = delete;

  void run(const AISnapshot &snapshot, AIContext &context, float deltaTime,
           AIBehaviorRegistry &registry, std::vector<AICommand> &outCommands);
};

} // namespace Game::Systems::AI
