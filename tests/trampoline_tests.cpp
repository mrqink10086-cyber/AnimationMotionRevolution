#include "SKSE/API.h"
#include "HookInstallation.h"

#include <Windows.h>
#include <cstdio>
#include <stdexcept>

namespace
{
	std::byte* memory;
	std::size_t poolRequests;
	std::size_t requestedSize;

	void Check(bool a_condition)
	{
		if (!a_condition) {
			throw std::runtime_error("Trampoline regression check failed");
		}
	}

	void* Allocate(SKSE::PluginHandle, std::size_t a_size)
	{
		++poolRequests;
		requestedSize = a_size;
		return a_size <= 4096 ? memory + 4096 : nullptr;
	}

	void* Query(std::uint32_t a_id)
	{
		static SKSE::Impl::SKSETrampolineInterface trampoline{ 1, Allocate, Allocate };
		return a_id == SKSE::LoadInterface::kTrampoline ? &trampoline : nullptr;
	}

	const SKSE::LoadInterface* Interface()
	{
		static SKSE::Impl::SKSEInterface api{
			REL::Version{ 2, 2, 6, 0 }.pack(), REL::Version{ 1, 6, 1170, 0 }.pack(), 0, 0,
			Query, []() -> SKSE::PluginHandle { return 1; }, []() -> std::uint32_t { return 1; },
			[](const char*) -> const void* { return nullptr; }
		};
		return reinterpret_cast<const SKSE::LoadInterface*>(&api);
	}

	void LegacyInstallation()
	{
		SKSE::Init(Interface(), { .log = false });
		const auto site = reinterpret_cast<std::uintptr_t>(memory);
		memory[0] = std::byte{ 0xE8 };
		SKSE::AllocTrampoline(utils::GetTrampolineBaseSize<5>());
		SKSE::GetTrampoline().write_call<5>(site, site + 1024);
		struct Code : Xbyak::CodeGenerator
		{
			Code()
			{
				mov(rdx, r15);
				jmp(ptr[rip]);
				dq(0x12345678);
			}
		} code;
		code.ready();
		memory[32] = std::byte{ 0xE8 };
		SKSE::AllocTrampoline(utils::GetTrampolineBaseSize<5>() + code.getSize());
		Check(poolRequests == 1 && SKSE::GetTrampoline().capacity() == 14 && SKSE::GetTrampoline().free_size() == 0);
		(void)SKSE::GetTrampoline().allocate(code);
	}

	std::uintptr_t PatchedTarget(std::uintptr_t a_site, std::uint8_t a_opcode = 0xE8)
	{
		Check(*reinterpret_cast<const std::uint8_t*>(a_site) == a_opcode);
		std::int32_t displacement;
		std::memcpy(&displacement, reinterpret_cast<const void*>(a_site + 1), sizeof(displacement));
		const auto bridge = a_site + 5 + displacement;
		Check(*reinterpret_cast<const std::uint16_t*>(bridge) == 0x25FF);
		std::uintptr_t target;
		std::memcpy(&target, reinterpret_cast<const void*>(bridge + 6), sizeof(target));
		return target;
	}

	void FullInstallation()
	{
		const auto base = reinterpret_cast<std::uintptr_t>(memory);
		amr::HookInstallation installation(base + 1024, memory + 1040, memory + 1056, memory + 1072, memory + 1088,
			base + 1104, base + 1120);
		const auto required = installation.RequiredSize();
		Check(required < 1024);
		memory[4096 + required] = std::byte{ 0x5A };
		SKSE::Init(Interface(), { .log = false, .trampoline = true, .trampolineSize = required });
		Check(poolRequests == 1 && requestedSize == required && SKSE::GetTrampoline().capacity() == required);

		std::array<std::uintptr_t, 16> sites;
		for (std::size_t i = 0; i < sites.size(); ++i) {
			sites[i] = base + i * 32;
			memory[i * 32] = i == 15 ? std::byte{ 0xE9 } : std::byte{ 0xE8 };
			const auto original = i < 7 ? base + 2048 + i * 16 : base + (i < 13 ? 2304 : 2320);
			const auto displacement = static_cast<std::int32_t>(original - sites[i] - 5);
			std::memcpy(memory + i * 32 + 1, &displacement, sizeof(displacement));
		}
		std::array<RUNTIME_FUNCTION, 4> tables{};
		const auto originals = installation.Install(SKSE::GetTrampoline(), {
			sites[0], sites[1], sites[2], { sites[3], sites[4], sites[5], sites[6] },
			{ sites[7], sites[8], sites[9], sites[10], sites[11], sites[12] },
			{ sites[13], sites[14] }, sites[15]
		}, tables);
		Check(originals.activate == base + 2064 && originals.deactivate == base + 2080);
		Check(originals.graphCreate == base + 2304 && originals.graphDestroy == base + 2320);
		Check(PatchedTarget(sites[0]) == base + 1024);
		for (std::size_t i = 1; i < 7; ++i) {
			const auto target = PatchedTarget(sites[i]);
			Check(target >= base + 4096 && target < base + 4096 + required);
			if (i >= 3) {
				Check((target & 15) == 0);
				DWORD64 foundBase = 0;
				Check(RtlLookupFunctionEntry(target + 4, &foundBase, nullptr) == &tables[i - 3] && foundBase == target);
			}
		}
		for (std::size_t i = 7; i < 15; ++i) {
			Check(PatchedTarget(sites[i]) == base + (i < 13 ? 1104 : 1120));
		}
		Check(PatchedTarget(sites[15], 0xE9) == base + 1120);
		Check(SKSE::GetTrampoline().allocated_size() == required && poolRequests == 1);
		Check(memory[4096 + required] == std::byte{ 0x5A });
		for (auto& table : tables) {
			Check(RtlDeleteFunctionTable(&table) != 0);
		}
		std::printf("16 sites installed; %zu / %zu bytes, %zu pool request, 4 unwind tables verified\n",
			SKSE::GetTrampoline().allocated_size(), requestedSize, poolRequests);
	}
}

int main(int a_argc, char**)
{
	memory = static_cast<std::byte*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!memory) {
		return 2;
	}
	const bool legacy = a_argc > 1;
	try {
		if (legacy) {
			LegacyInstallation();
		} else {
			FullInstallation();
		}
	} catch (const std::runtime_error&) {
		std::printf("%s: pool requests=%zu, capacity=%zu, used=%zu\n", legacy ? "Legacy allocation failure reproduced" : "FAIL",
			poolRequests, requestedSize, SKSE::GetTrampoline().allocated_size());
		return legacy && poolRequests == 1 && requestedSize == 14 && SKSE::GetTrampoline().allocated_size() == 14 ? 0 : 1;
	}
	if (legacy) {
		return 1;
	}
	std::puts("Trampoline installation passed");
}
