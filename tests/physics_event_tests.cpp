#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace RE
{
	struct Actor
	{};

	struct BSFixedString
	{
		BSFixedString() = default;
		BSFixedString(const char* a_value) : value(a_value && *a_value ? a_value : nullptr) {}

		bool empty() const noexcept { return !value || *value == '\0'; }

		const char* value = nullptr;
	};

	struct BSAnimationGraphEvent
	{
		const BSFixedString tag;
		Actor* holder = nullptr;
		const BSFixedString payload;
	};
}

#include "PhysicsEvents.h"

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::cerr << "check failed: " #condition ", file " << __FILE__ << ", line " << __LINE__ << '\n'; \
			std::abort(); \
		} \
	} while (false)

namespace
{
	std::size_t SimulateActorMediatorTagScan(const RE::BSAnimationGraphEvent& a_event)
	{
		const char* tag = a_event.tag.value;
		std::size_t length = 0;
		while (tag[length] != '\0') {
			++length;
		}
		return length;
	}

	void TestPhysicsUpdateEventHasReadableTagAndActor()
	{
		RE::Actor actor;
		const auto event = amr::MakePhysicsUpdateEvent(&actor);

		CHECK(event.tag.value != nullptr);
		CHECK(std::string_view(event.tag.value) == "AMR_PhysicsUpdate");
		CHECK(SimulateActorMediatorTagScan(event) == std::string_view("AMR_PhysicsUpdate").size());
		CHECK(event.holder == &actor);
		CHECK(event.payload.empty());
	}
}

int main()
{
	TestPhysicsUpdateEventHasReadableTagAndActor();
	std::cout << "physics update event fields verified\n";
}
