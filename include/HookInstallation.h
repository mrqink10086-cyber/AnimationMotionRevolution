#pragma once

#include "MotionHookCode.h"
#include "utils/Trampoline.h"

#include <Windows.h>
#include <array>

namespace amr
{
	class HookInstallation
	{
		struct ContextHookCode : Xbyak::CodeGenerator
		{
			ContextHookCode(void* a_function, const Xbyak::Reg64& a_context)
			{
				mov(rdx, a_context);
				jmp(ptr[rip]);
				dq(reinterpret_cast<std::uintptr_t>(a_function));
				ready();
			}
		};

	public:
		struct Sites
		{
			std::uintptr_t state;
			std::uintptr_t activate;
			std::uintptr_t deactivate;
			std::array<std::uintptr_t, 4> motion;
		};

		struct Originals
		{
			std::uintptr_t activate;
			std::uintptr_t deactivate;
		};

		HookInstallation(std::uintptr_t a_state, void* a_activate, void* a_deactivate, void* a_translation, void* a_rotation) :
			state(a_state), activate(a_activate, Xbyak::util::r15), deactivate(a_deactivate, Xbyak::util::rsi),
			translation(a_translation), rotation(a_rotation)
		{
			translation.ready();
			rotation.ready();
		}

		[[nodiscard]] std::size_t RequiredSize() const
		{
			return 7 * utils::GetTrampolineBaseSize<5>() + activate.getSize() + deactivate.getSize() +
				2 * (translation.getSize() + alignmentPadding) + 2 * (rotation.getSize() + alignmentPadding);
		}

		Originals Install(SKSE::Trampoline& a_trampoline, const Sites& a_sites, std::array<RUNTIME_FUNCTION, 4>& a_tables)
		{
			if (a_trampoline.free_size() < RequiredSize()) {
				RE::stl::report_and_fail("Insufficient reserved trampoline space for AMR hooks");
			}
			a_trampoline.write_call<5>(a_sites.state, state);
			const Originals originals{
				utils::WriteCallTrampoline<5>(a_trampoline, a_sites.activate, activate),
				utils::WriteCallTrampoline<5>(a_trampoline, a_sites.deactivate, deactivate)
			};
			for (std::size_t i = 0; i < a_tables.size(); ++i) {
				auto& code = i < 2 ? translation : rotation;
				const auto allocation = reinterpret_cast<std::uintptr_t>(a_trampoline.allocate(code.getSize() + alignmentPadding));
				const auto address = (allocation + alignmentPadding) & ~std::uintptr_t{ alignmentPadding };
				std::memcpy(reinterpret_cast<void*>(address), code.getCode(), code.getSize());
				a_tables[i] = { 0, code.bodySize, code.unwindOffset };
				if (!RtlAddFunctionTable(&a_tables[i], 1, address)) {
					RE::stl::report_and_fail("Cannot register motion hook unwind information");
				}
				a_trampoline.write_call<5>(a_sites.motion[i], address);
			}
			return originals;
		}

	private:
		static constexpr std::size_t alignmentPadding = 15;
		std::uintptr_t state;
		ContextHookCode activate;
		ContextHookCode deactivate;
		MotionHookCode translation;
		MotionHookCode rotation;
	};
}
