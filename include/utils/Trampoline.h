#pragma once

#if defined(SKSE_SUPPORT_XBYAK)

#include <xbyak/xbyak.h>

#include "SKSE/Trampoline.h"

namespace utils
{
	template <std::size_t N>
	static constexpr std::size_t GetTrampolineBaseSize()
	{
		static_assert(N == 5 || N == 6);

		// Reference: write_5branch() and write_6branch() of Trampoline.h
		if constexpr (N == 5) {
#pragma pack(push, 1)
			// FF /4
			// JMP r/m64
			struct TrampolineAssembly
			{
				// jmp [rip]
				std::uint8_t jmp;	 // 0 - 0xFF
				std::uint8_t modrm;	 // 1 - 0x25
				std::int32_t disp;	 // 2 - 0x00000000
				std::uint64_t addr;	 // 6 - [rip]
			};
#pragma pack(pop)

			return sizeof(TrampolineAssembly);
		} else {
			return sizeof(std::uintptr_t);
		}
	}

	template <std::size_t N>
	static std::uintptr_t WriteCallTrampoline(SKSE::Trampoline& a_trampoline, std::uintptr_t a_location, Xbyak::CodeGenerator& a_hookCode)
	{
		return a_trampoline.write_call<N>(a_location, a_trampoline.allocate(a_hookCode));
	}
}

#else
#error "Xbyak support needed for Trampoline"
#endif
