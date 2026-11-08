#include "MotionHookCode.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void Check(bool a_value)
{
	if (!a_value) {
		std::abort();
	}
}

static std::uintptr_t received[5]{};
static void Target(void* a_container, float a_time, void* a_output, void* a_clip, void* a_actor)
{
	received[0] = reinterpret_cast<std::uintptr_t>(a_container);
	received[1] = a_time == 1.25F;
	received[2] = reinterpret_cast<std::uintptr_t>(a_output);
	received[3] = reinterpret_cast<std::uintptr_t>(a_clip);
	received[4] = reinterpret_cast<std::uintptr_t>(a_actor);
}

int main()
{
	amr::MotionHookCode code(reinterpret_cast<void*>(&Target));
	code.ready();
	RUNTIME_FUNCTION function{ 0, code.bodySize, code.unwindOffset };
	const auto base = reinterpret_cast<DWORD64>(code.getCode());
	Check(RtlAddFunctionTable(&function, 1, base) != 0);
	DWORD64 foundBase = 0;
	Check(RtlLookupFunctionEntry(base + 4, &foundBase, nullptr) == &function && foundBase == base);

	alignas(16) std::array<DWORD64, 16> stack{};
	constexpr DWORD64 returnAddress = 0x1234567890;
	stack[5] = returnAddress;
	CONTEXT context{};
	context.Rip = base + 4;
	context.Rsp = reinterpret_cast<DWORD64>(stack.data());
	PVOID handlerData = nullptr;
	DWORD64 frame = 0;
	RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, &function, &context, &handlerData, &frame, nullptr);
	Check(context.Rip == returnAddress && context.Rsp == reinterpret_cast<DWORD64>(stack.data() + 6));

	context = {};
	context.Rip = base;
	context.Rsp = reinterpret_cast<DWORD64>(&stack[5]);
	RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, &function, &context, &handlerData, &frame, nullptr);
	Check(context.Rip == returnAddress && context.Rsp == reinterpret_cast<DWORD64>(stack.data() + 6));

	struct Caller : Xbyak::CodeGenerator
	{
		explicit Caller(std::uintptr_t a_target)
		{
			push(rbx);
			push(r13);
			sub(rsp, 0x28);
			mov(rcx, 0x1111);
			mov(eax, 0x3fa00000);
			movd(xmm1, eax);
			mov(r8, 0x2222);
			mov(rbx, 0x3333 + 0x14);
			mov(r13, 0x4444);
			mov(rax, a_target);
			call(rax);
			add(rsp, 0x28);
			pop(r13);
			pop(rbx);
			ret();
			ready();
		}
	} caller(base);
	caller.getCode<void (*)()>()();
	Check(received[0] == 0x1111 && received[1] == 1 && received[2] == 0x2222 &&
		  received[3] == 0x3333 && received[4] == 0x4444);
	Check(RtlDeleteFunctionTable(&function) != 0);
	std::puts("Motion trampoline ABI and Windows unwind tests passed");
}
