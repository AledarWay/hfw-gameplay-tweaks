#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "HRZ2/Core/RTTI.h"
#include "HRZ2/Core/RTTIRefObject.h"
#include "HRZ2/PCore/Common.h"
#include "ModConfiguration.h"
#include "SupercellTrace.h"

#pragma intrinsic(_ReturnAddress)

namespace SupercellTrace
{
	using namespace HRZ2;

	namespace
	{
		// RVAs in fullgame.dll for the HRZ2-PCR 80/9966125 build, resolved from graph entry point names
		struct CodeRange
		{
			uintptr_t m_Begin;
			uintptr_t m_End;
			const char *m_Name;
		};

		constexpr CodeRange Ranges[] = {
			{ 0xbceb948, 0xbcef000, "container" },
			{ 0x37291d8, 0x372c800, "limiter" },
			{ 0x463913c, 0x463a400, "weather" },
		};

		// Native fact API slots called by graph code
		constexpr uintptr_t SlotSetEnum = 0x10528170;
		constexpr uintptr_t SlotGetEnum = 0x10528178;
		constexpr uintptr_t SlotSetBool = 0x10528180;
		constexpr uintptr_t SlotGetBool = 0x10528190;
		constexpr uintptr_t SlotSetInt = 0x105281a0;
		constexpr uintptr_t SlotSetFloat = 0x105281b8;

		struct Handler
		{
			uintptr_t m_Rva;
			uint8_t m_Prologue[4];
			const char *m_Name;
		};

		constexpr Handler Handlers[] = {
			{ 0xbceb949, { 0x41, 0x57, 0x41, 0x56 }, "MsgBooleanFactChanged" },
			{ 0xbcebd74, { 0x56, 0x57, 0x53, 0x48 }, "MsgEnumFactChanged" },
			{ 0xbcebef9, { 0x41, 0x57, 0x41, 0x56 }, "MsgSceneDoneSpawning" },
			{ 0xbced198, { 0x56, 0x53, 0x48, 0x81 }, "MsgSceneEnterState" },
			{ 0xbced1f1, { 0x41, 0x57, 0x41, 0x56 }, "MsgSceneUpdate" },
		};

		using HandlerFn = uint64_t (*)(void *, void *, void *, void *, void *, void *);

		std::atomic_bool g_Installed = false;
		uintptr_t g_Base = 0;
		HandlerFn g_OriginalHandlers[std::size(Handlers)] = {};

		std::mutex g_LogMutex;
		std::unordered_set<std::string> g_SeenLines;
		std::unordered_map<void *, int> g_LastState;
		uint64_t g_UpdateCount = 0;

		const char *RangeName(void *ReturnAddress)
		{
			const auto rva = reinterpret_cast<uintptr_t>(ReturnAddress) - g_Base;

			for (const auto& r : Ranges)
			{
				if (rva >= r.m_Begin && rva < r.m_End)
					return r.m_Name;
			}

			return nullptr;
		}

		bool SafeReadUUID(const void *Object, GGUUID *Out)
		{
			__try
			{
				memcpy(Out, static_cast<const uint8_t *>(Object) + 8, sizeof(GGUUID));
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool SafeReadInt(const void *Address, int *Out)
		{
			__try
			{
				*Out = *static_cast<const int *>(Address);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string Describe(const void *Object)
		{
			GGUUID uuid {};

			if (!Object)
				return "null";

			if (!SafeReadUUID(Object, &uuid))
				return std::format("{}", Object);

			return std::format("{}", uuid);
		}

		// Only first occurrence of each distinct line, so per-frame calls don't flood the log
		void LogOnce(std::string Line)
		{
			std::scoped_lock lock(g_LogMutex);

			if (g_SeenLines.size() < 20000 && g_SeenLines.emplace(Line).second)
				spdlog::info("[trace] {}", Line);
		}

		// Fact API wrappers
		bool (*g_GetBool)(void *, void *) = nullptr;
		void *(*g_GetEnum)(void *, void *) = nullptr;
		uint64_t (*g_SetBool)(void *, void *, bool) = nullptr;
		uint64_t (*g_SetEnum)(void *, void *, void *) = nullptr;
		uint64_t (*g_SetInt)(void *, void *, int) = nullptr;
		uint64_t (*g_SetFloat)(void *, void *, float) = nullptr;

		struct BoolOverride
		{
			GGUUID m_Fact;
			bool m_Value;
		};

		// Values the supercell scripts see after the AETHER merge, to match a pre-merge save
		constexpr BoolOverride SupercellOverrides[] = {
			{ GGUUID::Parse("E5E98601-17CB-644B-975B-E1EB6CB9571C"), false }, // Supercells disabled (set on merge)
			{ GGUUID::Parse("5FB621F0-9A0E-3040-9D75-7D48CF594FAB"), true },  // Supercell weather enabled (cleared on merge)
			{ GGUUID::Parse("095C0A02-BBF4-CB40-9E67-85E355006361"), false }, // Read by the lightning effects, true only after merge
		};

		bool HookGetBool(void *Context, void *Fact)
		{
			auto result = g_GetBool(Context, Fact);
			const auto range = RangeName(_ReturnAddress());

			if (!range)
				return result;

			GGUUID uuid {};

			if (ModConfiguration.KeepSupercellsAfterAether && SafeReadUUID(Fact, &uuid))
			{
				for (const auto& o : SupercellOverrides)
				{
					if (uuid == o.m_Fact)
						result = o.m_Value;
				}
			}

			if (ModConfiguration.SupercellTrace)
				LogOnce(std::format("{} GetBool {} -> {}", range, Describe(Fact), result));

			return result;
		}

		// Quest whose completion makes the limiter suppress supercells (late game); reported as not started instead
		constexpr auto LateQuestStateFact = GGUUID::Parse("05D97477-7F4F-F94E-9D85-F5954FC042B9");

		void *DefaultEnumEntry(void *Fact)
		{
			static std::atomic<void *> cached = nullptr;

			if (auto entry = cached.load())
				return entry;

			try
			{
				auto fact = static_cast<RTTIRefObject *>(Fact);
				const auto defaultUUID = fact->GetMemberRefUnsafe<GGUUID>("DefaultValueUUID");
				auto definition = fact->GetMemberRefUnsafe<Ref<RTTIRefObject>>("EnumDefinition").GetPtr();

				for (auto& entry : definition->GetMemberRefUnsafe<Array<Ref<RTTIRefObject>>>("EnumValues"))
				{
					if (entry.GetPtr() && entry->m_UUID == defaultUUID)
					{
						cached = entry.GetPtr();
						return entry.GetPtr();
					}
				}
			}
			catch (const std::exception&)
			{
			}

			return nullptr;
		}

		void *HookGetEnum(void *Context, void *Fact)
		{
			auto result = g_GetEnum(Context, Fact);

			if (ModConfiguration.KeepSupercellsAfterAether && RangeName(_ReturnAddress()))
			{
				GGUUID uuid {};

				if (SafeReadUUID(Fact, &uuid) && uuid == LateQuestStateFact)
				{
					if (auto entry = DefaultEnumEntry(Fact))
						result = entry;
				}
			}

			if (auto range = ModConfiguration.SupercellTrace ? RangeName(_ReturnAddress()) : nullptr)
				LogOnce(std::format("{} GetEnum {} -> {}", range, Describe(Fact), Describe(result)));

			return result;
		}

		uint64_t HookSetBool(void *Context, void *Fact, bool Value)
		{
			if (auto range = ModConfiguration.SupercellTrace ? RangeName(_ReturnAddress()) : nullptr)
				LogOnce(std::format("{} SetBool {} = {}", range, Describe(Fact), Value));

			return g_SetBool(Context, Fact, Value);
		}

		uint64_t HookSetEnum(void *Context, void *Fact, void *Value)
		{
			if (auto range = ModConfiguration.SupercellTrace ? RangeName(_ReturnAddress()) : nullptr)
				LogOnce(std::format("{} SetEnum {} = {}", range, Describe(Fact), Describe(Value)));

			return g_SetEnum(Context, Fact, Value);
		}

		uint64_t HookSetInt(void *Context, void *Fact, int Value)
		{
			if (auto range = ModConfiguration.SupercellTrace ? RangeName(_ReturnAddress()) : nullptr)
				LogOnce(std::format("{} SetInt {} = {}", range, Describe(Fact), Value));

			return g_SetInt(Context, Fact, Value);
		}

		uint64_t HookSetFloat(void *Context, void *Fact, float Value)
		{
			if (auto range = ModConfiguration.SupercellTrace ? RangeName(_ReturnAddress()) : nullptr)
				LogOnce(std::format("{} SetFloat {} = {}", range, Describe(Fact), Value));

			return g_SetFloat(Context, Fact, Value);
		}

		// The state machine index lives at State + 0x28 in every handler
		template<size_t Index>
		uint64_t HookHandler(void *A, void *B, void *State, void *Instance, void *E, void *F)
		{
			int before = -1;
			int after = -1;
			SafeReadInt(static_cast<uint8_t *>(State) + 0x28, &before);

			const auto result = g_OriginalHandlers[Index](A, B, State, Instance, E, F);
			SafeReadInt(static_cast<uint8_t *>(State) + 0x28, &after);

			std::scoped_lock lock(g_LogMutex);

			if constexpr (Index == std::size(Handlers) - 1)
			{
				auto [itr, inserted] = g_LastState.try_emplace(State, after);
				const bool changed = inserted || itr->second != after || before != after;
				itr->second = after;

				if (++g_UpdateCount % 3000 == 1 || changed)
					spdlog::info("[trace] container {} #{} state {} -> {} (instance {})", Handlers[Index].m_Name, g_UpdateCount, before, after, Instance);
			}
			else
			{
				spdlog::info("[trace] container {} state {} -> {} result {}", Handlers[Index].m_Name, before, after, result);
			}

			return result;
		}

		template<size_t... I>
		constexpr auto MakeHandlerHooks(std::index_sequence<I...>)
		{
			return std::array<HandlerFn, sizeof...(I)> { &HookHandler<I>... };
		}

		constexpr auto HandlerHooks = MakeHandlerHooks(std::make_index_sequence<std::size(Handlers)>());

		template<typename T>
		bool SwapSlot(uintptr_t Rva, T *Hook, T **Original)
		{
			auto slot = reinterpret_cast<T **>(g_Base + Rva);

			if (!*slot)
				return false;

			*Original = *slot;
			*slot = Hook;
			return true;
		}
	}

	void TryInstall()
	{
		if ((!ModConfiguration.SupercellTrace && !ModConfiguration.KeepSupercellsAfterAether) || g_Installed.load())
			return;

		const auto module = GetModuleHandleW(L"fullgame.dll");

		if (!module)
			return;

		g_Base = reinterpret_cast<uintptr_t>(module);

		// Slots are filled by the exe after the library loads
		if (!*reinterpret_cast<void **>(g_Base + SlotGetBool))
			return;

		if (g_Installed.exchange(true))
			return;

		for (const auto& h : Handlers)
		{
			if (memcmp(reinterpret_cast<void *>(g_Base + h.m_Rva), h.m_Prologue, sizeof(h.m_Prologue)) != 0)
			{
				spdlog::error("[trace] fullgame.dll does not match the expected build ({}), tracing disabled", h.m_Name);
				return;
			}
		}

		bool ok = SwapSlot(SlotGetBool, &HookGetBool, &g_GetBool) && SwapSlot(SlotGetEnum, &HookGetEnum, &g_GetEnum) &&
				  SwapSlot(SlotSetBool, &HookSetBool, &g_SetBool) && SwapSlot(SlotSetEnum, &HookSetEnum, &g_SetEnum) &&
				  SwapSlot(SlotSetInt, &HookSetInt, &g_SetInt) && SwapSlot(SlotSetFloat, &HookSetFloat, &g_SetFloat);

		if (ModConfiguration.SupercellTrace)
		{
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());

			for (size_t i = 0; i < std::size(Handlers); i++)
			{
				g_OriginalHandlers[i] = reinterpret_cast<HandlerFn>(g_Base + Handlers[i].m_Rva);
				DetourAttach(reinterpret_cast<void **>(&g_OriginalHandlers[i]), reinterpret_cast<void *>(HandlerHooks[i]));
			}

			ok = DetourTransactionCommit() == NO_ERROR && ok;
		}

		spdlog::info(
			"[supercells] hooks installed at {:#x}: {} (keep after AETHER: {}, trace: {})",
			g_Base,
			ok ? "ok" : "PARTIAL",
			ModConfiguration.KeepSupercellsAfterAether,
			ModConfiguration.SupercellTrace);
	}
}
