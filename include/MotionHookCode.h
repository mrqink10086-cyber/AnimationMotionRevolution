#pragma once

#include <cstdint>
#include <xbyak/xbyak.h>

namespace amr
{
	struct MotionHookCode : Xbyak::CodeGenerator
	{
		std::uint32_t bodySize;
		std::uint32_t unwindOffset;

		explicit MotionHookCode(void* a_function)
		{
			Xbyak::Label hookLabel;
			sub(rsp, 0x28);
			const auto prologSize = static_cast<std::uint8_t>(getSize());
			mov(ptr[rsp + 0x20], r13);
			lea(r9, ptr[rbx - 0x14]);
			call(ptr[rip + hookLabel]);
			add(rsp, 0x28);
			ret();
			bodySize = static_cast<std::uint32_t>(getSize());
			L(hookLabel), dq(reinterpret_cast<std::uintptr_t>(a_function));
			align(4);
			unwindOffset = static_cast<std::uint32_t>(getSize());
			db(0x01);
			db(prologSize);
			db(0x01);
			db(0x00);
			db(prologSize);
			db(0x42);
			db(0x00);
			db(0x00);
		}
	};
}
