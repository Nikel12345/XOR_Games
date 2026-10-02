#pragma once
#include "Aliases.h"

class EngineContext;

namespace SandboxScene
{
	void CreateModels(EngineContext* ctx);
	void DeleteModels(EngineContext* ctx);
	void CreateDebugColliders(EngineContext* ctx, const SceneName& scene_name);
	void BuildUI(EngineContext* ctx);
}
