#pragma once

namespace amr
{
	inline RE::BSAnimationGraphEvent MakePhysicsUpdateEvent(RE::Actor* a_actor)
	{
		return { "AMR_PhysicsUpdate", a_actor, "" };
	}
}
