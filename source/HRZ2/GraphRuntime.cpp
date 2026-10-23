#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>
#include <algorithm>
#include <tuple>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include "../ModConfiguration.h"
#include "../ModCoreEvents.h"
#include "Core/Entity.h"
#include "Core/ProgramResource.h"
#include "Core/RTTI.h"
#include "Core/RTTIRefObject.h"
#include "GraphRuntime.h"

#pragma intrinsic(_ReturnAddress)

namespace HRZ2::GraphRuntime
{
	namespace
	{
		//
		// fullgame.dll exports every graph entry point under MurmurHash3_x64_128(name, seed 42)
		//
		constexpr uint64_t Rotl(uint64_t X, int R)
		{
			return (X << R) | (X >> (64 - R));
		}

		constexpr uint64_t Fmix(uint64_t K)
		{
			K ^= K >> 33;
			K *= 0xff51afd7ed558ccdull;
			K ^= K >> 33;
			K *= 0xc4ceb9fe1a85ec53ull;
			K ^= K >> 33;
			return K;
		}

		std::array<uint64_t, 2> Murmur3(std::string_view Data, uint64_t Seed)
		{
			constexpr uint64_t c1 = 0x87c37b91114253d5ull;
			constexpr uint64_t c2 = 0x4cf5ad432745937full;

			const auto bytes = reinterpret_cast<const uint8_t *>(Data.data());
			const size_t length = Data.size();
			const size_t blocks = length / 16;
			uint64_t h1 = Seed;
			uint64_t h2 = Seed;

			for (size_t i = 0; i < blocks; i++)
			{
				uint64_t k1;
				uint64_t k2;
				memcpy(&k1, bytes + i * 16, 8);
				memcpy(&k2, bytes + i * 16 + 8, 8);

				k1 *= c1, k1 = Rotl(k1, 31), k1 *= c2, h1 ^= k1;
				h1 = Rotl(h1, 27), h1 += h2, h1 = h1 * 5 + 0x52dce729;
				k2 *= c2, k2 = Rotl(k2, 33), k2 *= c1, h2 ^= k2;
				h2 = Rotl(h2, 31), h2 += h1, h2 = h2 * 5 + 0x38495ab5;
			}

			const auto tail = bytes + blocks * 16;
			const size_t rest = length & 15;
			uint64_t k1 = 0;
			uint64_t k2 = 0;

			for (size_t i = rest; i > 8; i--)
				k2 ^= uint64_t(tail[i - 1]) << ((i - 9) * 8);

			if (rest > 8)
				k2 *= c2, k2 = Rotl(k2, 33), k2 *= c1, h2 ^= k2;

			for (size_t i = std::min<size_t>(rest, 8); i > 0; i--)
				k1 ^= uint64_t(tail[i - 1]) << ((i - 1) * 8);

			if (rest > 0)
				k1 *= c1, k1 = Rotl(k1, 31), k1 *= c2, h1 ^= k1;

			h1 ^= length, h2 ^= length;
			h1 += h2, h2 += h1;
			h1 = Fmix(h1), h2 = Fmix(h2);
			h1 += h2, h2 += h1;

			return { h1, h2 };
		}

		struct ExportEntry
		{
			uint64_t m_Hash[2];
			void *m_Function;
		};

		struct HashKey
		{
			size_t operator()(const std::array<uint64_t, 2>& K) const
			{
				return static_cast<size_t>(K[0] ^ (K[1] * 31));
			}
		};

		//
		// Native fact API slots in fullgame.dll's .data (HRZ2-PCR 80/9966125). Filled by the exe at load.
		//
		constexpr uintptr_t SlotSetPosition = 0x10528148;
		constexpr uintptr_t SlotGetPosition = 0x10528150;
		constexpr uintptr_t SlotSetEnum = 0x10528170;
		constexpr uintptr_t SlotGetEnum = 0x10528178;
		constexpr uintptr_t SlotSetBool = 0x10528180;
		constexpr uintptr_t SlotGetBool = 0x10528190;
		constexpr uintptr_t SlotSetInt = 0x105281a0;
		constexpr uintptr_t SlotGetInt = 0x105281a8;
		constexpr uintptr_t SlotSetFloat = 0x105281b8;
		constexpr uintptr_t SlotGetFloat = 0x105281c0;

		constexpr size_t MaxTags = 64;
		constexpr size_t MaxRanges = 256;

		struct CodeRange
		{
			uintptr_t m_Begin;
			uintptr_t m_End;
			size_t m_Tag;
		};

		struct ObservedEntry : ObservedFact
		{
			std::string m_LoggedValue;
			uint64_t m_LoggedTick = 0;
		};

		std::mutex g_Mutex;
		std::atomic_bool g_Installed = false;
		uintptr_t g_Base = 0;
		std::unordered_map<std::array<uint64_t, 2>, void *, HashKey> g_Exports;
		std::vector<uintptr_t> g_ExportAddresses;

		std::array<std::string, MaxTags> g_Tags;
		std::array<std::atomic_bool, MaxTags> g_TagTrace;
		std::array<size_t, MaxTags> g_TagEntryCount = {};
		std::atomic_size_t g_TagCount = 0;

		std::array<CodeRange, MaxRanges> g_Ranges = {};
		std::atomic_size_t g_RangeCount = 0;

		std::shared_mutex g_OverrideMutex;
		std::vector<FactOverride> g_Overrides;

		std::mutex g_ObservedMutex;
		std::map<std::pair<size_t, GGUUID>, ObservedEntry> g_Observed;

		std::mutex g_LabelMutex;
		std::unordered_map<std::string, std::string> g_Labels;

		// Facts watched for every caller
		constexpr size_t MaxWatchedFacts = 32;
		std::array<GGUUID, MaxWatchedFacts> g_WatchedFacts = {};
		std::atomic_size_t g_WatchedFactCount = 0;
		std::map<std::pair<std::string, GGUUID>, ObservedEntry> g_WatchObserved; // Guarded by g_ObservedMutex

		std::mutex g_NameMutex;
		std::unordered_map<uintptr_t, std::string> g_EntryNames;
		std::unordered_map<uintptr_t, std::string> g_CallerNames;
		bool g_EntryNamesBuilt = false;
		uint64_t g_EntryNamesTick = 0;

		uintptr_t g_ExeBase = 0;
		uintptr_t g_ExeEnd = 0;
		std::atomic_bool g_NativeInstalled = false;

		std::mutex g_EnumMutex;
		std::map<std::pair<void *, GGUUID>, void *> g_EnumEntryCache;

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

		const CodeRange *FindRange(void *ReturnAddress)
		{
			const auto address = reinterpret_cast<uintptr_t>(ReturnAddress);
			const auto count = g_RangeCount.load(std::memory_order_acquire);

			for (size_t i = 0; i < count; i++)
			{
				if (address >= g_Ranges[i].m_Begin && address < g_Ranges[i].m_End)
					return &g_Ranges[i];
			}

			return nullptr;
		}

		std::string Lower(std::string_view Text)
		{
			std::string result(Text);
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		bool TagMatches(const std::string& Tag, const std::vector<std::string>& Filters)
		{
			if (Filters.empty())
				return true;

			const auto tag = Lower(Tag);
			return std::ranges::any_of(Filters, [&](const auto& f) { return tag.find(Lower(f)) != std::string::npos; });
		}

		std::optional<OverrideValue> FindOverride(size_t Tag, const GGUUID& Fact, FactKind Kind)
		{
			std::shared_lock lock(g_OverrideMutex);

			for (const auto& o : g_Overrides)
			{
				if (o.m_Enabled && o.m_Fact == Fact && o.m_Value.m_Kind == Kind && TagMatches(g_Tags[Tag], o.m_Graphs))
					return o.m_Value;
			}

			return std::nullopt;
		}

		void *ResolveEnumEntry(void *Fact, const std::optional<GGUUID>& Entry)
		{
			const auto key = std::make_pair(Fact, Entry.value_or(GGUUID {}));
			std::scoped_lock lock(g_EnumMutex);

			if (auto itr = g_EnumEntryCache.find(key); itr != g_EnumEntryCache.end())
				return itr->second;

			void *result = nullptr;

			try
			{
				auto fact = static_cast<RTTIRefObject *>(Fact);
				const auto wanted = Entry.value_or(fact->GetMemberRefUnsafe<GGUUID>("DefaultValueUUID"));
				auto definition = fact->GetMemberRefUnsafe<Ref<RTTIRefObject>>("EnumDefinition").GetPtr();

				for (auto& e : definition->GetMemberRefUnsafe<Array<Ref<RTTIRefObject>>>("EnumValues"))
				{
					if (e.GetPtr() && e->m_UUID == wanted)
						result = e.GetPtr();
				}
			}
			catch (const std::exception&)
			{
			}

			g_EnumEntryCache.emplace(key, result);
			return result;
		}

		std::string EnumEntryName(void *Entry)
		{
			if (!Entry)
				return "null";

			GGUUID uuid {};

			if (!SafeReadUUID(Entry, &uuid))
				return std::format("{}", Entry);

			try
			{
				return std::format("{} ({})", static_cast<RTTIRefObject *>(Entry)->GetMemberRefUnsafe<String>("Name").c_str(), uuid);
			}
			catch (const std::exception&)
			{
				return std::format("{}", uuid);
			}
		}

		uint64_t ContextId(void *Context);

		// Traced graphs log a value whenever it differs from the last one logged for the same fact and context
		std::map<std::tuple<size_t, std::array<uint8_t, sizeof(GGUUID)>, uint64_t>, std::string> g_TraceLogged;

		void Observe(const CodeRange& Range, void *Context, void *Fact, FactKind Kind, bool Write, bool Overridden, std::string Value, float Number = 0, const double *Position = nullptr)
		{
			GGUUID uuid {};

			if (!SafeReadUUID(Fact, &uuid))
				return;

			const auto tick = GetTickCount64();
			std::scoped_lock lock(g_ObservedMutex);

			auto& entry = g_Observed[std::make_pair(Range.m_Tag, uuid)];

			if (entry.m_Count == 0)
			{
				entry.m_Graph = g_Tags[Range.m_Tag];
				entry.m_Fact = uuid;
				entry.m_Kind = Kind;
			}

			entry.m_Object = Fact;
			entry.m_LastWasWrite = Write;
			entry.m_Overridden = Overridden;
			entry.m_Number = Number;
			entry.m_Count++;
			entry.m_LastTick = tick;

			if (Position)
				std::copy_n(Position, 3, entry.m_Position);

			const bool trace = g_TagTrace[Range.m_Tag].load(std::memory_order_relaxed);
			const bool throttled = Kind == FactKind::Float || Kind == FactKind::Position;

			if (trace && (!throttled || tick - entry.m_LoggedTick > 2000))
			{
				const auto context = ContextId(Context);
				std::array<uint8_t, sizeof(GGUUID)> key {};
				memcpy(key.data(), &uuid, key.size());
				auto& logged = g_TraceLogged[std::make_tuple(Range.m_Tag, key, context)];

				if (logged != Value)
				{
					spdlog::info(
						"[graph] {} {} {} {} {}{} ctx={:#x}",
						entry.m_Graph,
						Write ? "Set" : "Get",
						KindName(Kind),
						GetFactLabel(uuid),
						Value,
						Overridden ? " (overridden)" : "",
						context);

					logged = Value;
					entry.m_LoggedValue = Value;
					entry.m_LoggedTick = tick;
				}
			}

			if (entry.m_ChangedTick == 0 || entry.m_Value != Value)
				entry.m_ChangedTick = tick;

			entry.m_Value = std::move(Value);
		}

		void BuildEntryNamesLocked()
		{
			auto& events = ModCoreEvents::GetInstance();
			std::shared_lock lock(events.m_CachedDataMutex);

			for (auto graph : events.m_CachedGraphPrograms)
			{
				for (const auto& entry : static_cast<ProgramResource *>(graph)->m_EntryPoints)
				{
					const std::string_view name(entry.m_EntryPoint.c_str());

					if (auto itr = g_Exports.find(Murmur3(name, 42)); itr != g_Exports.end())
						g_EntryNames.emplace(reinterpret_cast<uintptr_t>(itr->second), std::string(name));
				}
			}

			g_EntryNamesBuilt = true;
			g_EntryNamesTick = GetTickCount64();
		}

		std::string CallerName(void *ReturnAddress)
		{
			const auto address = reinterpret_cast<uintptr_t>(ReturnAddress);

			if (address >= g_ExeBase && address < g_ExeEnd)
				return std::format("native+{:#x}", address - g_ExeBase);

			std::scoped_lock lock(g_NameMutex);

			if (auto itr = g_CallerNames.find(address); itr != g_CallerNames.end())
				return itr->second;

			// Graph code: the closest exported entry point at or below the caller
			std::string name;
			auto next = std::upper_bound(g_ExportAddresses.begin(), g_ExportAddresses.end(), address);

			if (next != g_ExportAddresses.begin())
			{
				const auto function = *(next - 1);
				auto itr = g_EntryNames.find(function);

				if (itr == g_EntryNames.end() && (!g_EntryNamesBuilt || GetTickCount64() - g_EntryNamesTick > 10000))
				{
					BuildEntryNamesLocked();
					itr = g_EntryNames.find(function);
				}

				if (itr != g_EntryNames.end())
					name = itr->second;
			}

			if (name.empty())
				name = std::format("fullgame+{:#x}", address - g_Base);

			g_CallerNames.emplace(address, name);
			return name;
		}

		struct WatchCall
		{
			std::string m_Caller;
			GGUUID m_Fact;
		};

		std::optional<WatchCall> WatchedCall(void *ReturnAddress, void *Fact)
		{
			const auto count = g_WatchedFactCount.load(std::memory_order_acquire);
			GGUUID uuid {};

			if (count == 0 || !SafeReadUUID(Fact, &uuid))
				return std::nullopt;

			for (size_t i = 0; i < count; i++)
			{
				if (g_WatchedFacts[i] == uuid)
					return WatchCall { CallerName(ReturnAddress), uuid };
			}

			return std::nullopt;
		}

		std::optional<OverrideValue> FindCallerOverride(const WatchCall& Call, FactKind Kind)
		{
			std::shared_lock lock(g_OverrideMutex);

			for (const auto& o : g_Overrides)
			{
				if (o.m_Enabled && !o.m_Graphs.empty() && o.m_Fact == Call.m_Fact && o.m_Value.m_Kind == Kind && TagMatches(Call.m_Caller, o.m_Graphs))
					return o.m_Value;
			}

			return std::nullopt;
		}

		uint64_t ContextId(void *Context)
		{
			uint64_t id = 0;
			__try
			{
				memcpy(&id, Context, sizeof(id));
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			return id;
		}

		void ObserveWatched(const WatchCall& Call, void *Context, void *Fact, FactKind Kind, bool Write, bool Overridden, std::string Value, float Number = 0, const double *Position = nullptr)
		{
			const auto tick = GetTickCount64();
			const auto context = ContextId(Context);
			std::scoped_lock lock(g_ObservedMutex);

			auto& entry = g_WatchObserved[std::make_pair(Call.m_Caller, Call.m_Fact)];
			const bool first = entry.m_Count == 0;

			if (first)
			{
				entry.m_Graph = Call.m_Caller;
				entry.m_Fact = Call.m_Fact;
				entry.m_Kind = Kind;
			}

			entry.m_Object = Fact;
			entry.m_LastWasWrite = Write;
			entry.m_Overridden = Overridden;
			entry.m_Number = Number;
			entry.m_Count++;
			entry.m_LastTick = tick;

			if (Position)
				std::copy_n(Position, 3, entry.m_Position);

			const bool throttled = Kind == FactKind::Float || Kind == FactKind::Position;

			// Values differ per entity, so the context is logged to tell them apart
			if (first || (Value != entry.m_LoggedValue && (!throttled || tick - entry.m_LoggedTick > 2000)))
			{
				spdlog::info(
					"[fact] {} {} {} {} {}{} ctx={:#x}",
					Call.m_Caller,
					Write ? "Set" : "Get",
					KindName(Kind),
					GetFactLabel(Call.m_Fact),
					Value,
					Overridden ? " (overridden)" : "",
					context);

				entry.m_LoggedValue = Value;
				entry.m_LoggedTick = tick;
			}

			if (entry.m_ChangedTick == 0 || entry.m_Value != Value)
				entry.m_ChangedTick = tick;

			entry.m_Value = std::move(Value);
		}

		//
		// Fact API hooks
		//
		bool (*g_GetBool)(void *, void *);
		void *(*g_GetEnum)(void *, void *);
		int (*g_GetInt)(void *, void *);
		float (*g_GetFloat)(void *, void *);
		void *(*g_GetPosition)(void *, void *, void *);
		uint64_t (*g_SetBool)(void *, void *, bool);
		uint64_t (*g_SetEnum)(void *, void *, void *);
		uint64_t (*g_SetInt)(void *, void *, int);
		uint64_t (*g_SetFloat)(void *, void *, float);
		uint64_t (*g_SetPosition)(void *, void *, const void *);

		std::optional<OverrideValue> LookupOverride(const CodeRange& Range, void *Fact, FactKind Kind)
		{
			GGUUID uuid {};

			if (!SafeReadUUID(Fact, &uuid))
				return std::nullopt;

			return FindOverride(Range.m_Tag, uuid, Kind);
		}

		// Calls from code outside the watched graphs, for watched facts only
		// Logs write-backs at most every 2 s per fact, the game may keep restoring its own value
		void LogWriteBack(void *Context, void *Fact, const std::string& Value)
		{
			static std::mutex mutex;
			static std::map<void *, uint64_t> last;
			const auto now = GetTickCount64();
			GGUUID uuid {};

			{
				std::scoped_lock lock(mutex);
				auto& tick = last[Fact];

				if (now - tick < 2000)
					return;

				tick = now;
			}

			if (SafeReadUUID(Fact, &uuid))
				spdlog::info("[graph] write-back {} = {} ctx={:#x}", GetFactLabel(uuid), Value, ContextId(Context));
		}

		template<typename T, typename ApplyFn, typename FormatFn, typename WriteFn>
		T WatchGet(void *ReturnAddress, void *Context, void *Fact, FactKind Kind, T Result, ApplyFn Apply, FormatFn Format, WriteFn Write)
		{
			if (auto call = WatchedCall(ReturnAddress, Fact))
			{
				const auto o = FindCallerOverride(*call, Kind);
				const auto real = Result;

				if (o)
				{
					Apply(Result, *o);

					if (o->m_WriteBack && real != Result)
					{
						Write(Context, Fact, Result);
						LogWriteBack(Context, Fact, Format(Result));
					}
				}

				ObserveWatched(*call, Context, Fact, Kind, false, o.has_value(), Format(Result), 0);
			}

			return Result;
		}

		void WatchSet(void *ReturnAddress, void *Context, void *Fact, FactKind Kind, std::string Value, float Number = 0, const double *Position = nullptr)
		{
			if (auto call = WatchedCall(ReturnAddress, Fact))
				ObserveWatched(*call, Context, Fact, Kind, true, false, std::move(Value), Number, Position);
		}

		const auto ApplyBool = [](bool& R, const OverrideValue& O) { R = O.m_Bool; };
		const auto ApplyInt = [](int& R, const OverrideValue& O) { R = O.m_Int; };
		const auto ApplyFloat = [](float& R, const OverrideValue& O) { R = O.m_Float; };
		const auto FormatBool = [](bool R) { return std::string(R ? "true" : "false"); };
		const auto WriteBool = [](void *C, void *F, bool V) { g_SetBool(C, F, V); };
		const auto WriteInt = [](void *C, void *F, int V) { g_SetInt(C, F, V); };
		const auto WriteFloat = [](void *C, void *F, float V) { g_SetFloat(C, F, V); };
		const auto FormatInt = [](int R) { return std::to_string(R); };
		const auto FormatFloat = [](float R) { return std::format("{:.3f}", R); };

		void *ApplyEnumOverride(void *Fact, void *Result, std::optional<OverrideValue> O)
		{
			GGUUID current {};

			if (!O || (O->m_OnlyWhenEntry && (!Result || !SafeReadUUID(Result, &current) || current != *O->m_OnlyWhenEntry)))
				return Result;

			auto entry = ResolveEnumEntry(Fact, O->m_EnumEntry);
			return entry ? entry : Result;
		}

		void ApplyPositionOverride(double *Position, const std::optional<OverrideValue>& O)
		{
			if (!O)
				return;

			for (int i = 0; i < 3; i++)
			{
				if (O->m_PositionAxes[i])
					Position[i] = O->m_Position[i];
			}
		}

		void *WatchGetEnum(void *ReturnAddress, void *Context, void *Fact, void *Result)
		{
			if (auto call = WatchedCall(ReturnAddress, Fact))
			{
				const auto o = FindCallerOverride(*call, FactKind::Enum);
				auto value = ApplyEnumOverride(Fact, Result, o);

				if (o && o->m_WriteBack && value != Result)
				{
					g_SetEnum(Context, Fact, value);
					LogWriteBack(Context, Fact, EnumEntryName(value));
				}
				ObserveWatched(*call, Context, Fact, FactKind::Enum, false, value != Result, EnumEntryName(value));
				return value;
			}

			return Result;
		}

		void WatchGetPosition(void *ReturnAddress, void *Context, void *Fact, double *Position)
		{
			if (auto call = WatchedCall(ReturnAddress, Fact))
			{
				const auto o = FindCallerOverride(*call, FactKind::Position);
				ApplyPositionOverride(Position, o);
				ObserveWatched(
					*call,
					Context,
					Fact,
					FactKind::Position,
					false,
					o.has_value(),
					std::format("{:.0f}, {:.0f}, {:.0f}", Position[0], Position[1], Position[2]),
					0,
					Position);
			}
		}

		//
		// Slot hooks: calls from graph programs in fullgame.dll
		//
		bool HookGetBool(void *Context, void *Fact)
		{
			auto result = g_GetBool(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Bool);

				if (o)
				{
					if (o->m_WriteBack && result != o->m_Bool)
					{
						g_SetBool(Context, Fact, o->m_Bool);
						LogWriteBack(Context, Fact, o->m_Bool ? "true" : "false");
					}

					result = o->m_Bool;
				}

				Observe(*range, Context, Fact, FactKind::Bool, false, o.has_value(), result ? "true" : "false", result ? 1.0f : 0.0f);
				return result;
			}

			return WatchGet(_ReturnAddress(), Context, Fact, FactKind::Bool, result, ApplyBool, FormatBool, WriteBool);
		}

		void *HookGetEnum(void *Context, void *Fact)
		{
			auto result = g_GetEnum(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Enum);
				auto value = ApplyEnumOverride(Fact, result, o);

				if (o && o->m_WriteBack && value != result)
				{
					g_SetEnum(Context, Fact, value);
					LogWriteBack(Context, Fact, EnumEntryName(value));
				}

				Observe(*range, Context, Fact, FactKind::Enum, false, value != result, EnumEntryName(value));
				return value;
			}

			return WatchGetEnum(_ReturnAddress(), Context, Fact, result);
		}

		int HookGetInt(void *Context, void *Fact)
		{
			auto result = g_GetInt(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Int);

				if (o)
				{
					if (o->m_WriteBack && result != o->m_Int)
					{
						g_SetInt(Context, Fact, o->m_Int);
						LogWriteBack(Context, Fact, std::to_string(o->m_Int));
					}

					result = o->m_Int;
				}

				Observe(*range, Context, Fact, FactKind::Int, false, o.has_value(), std::to_string(result), static_cast<float>(result));
				return result;
			}

			return WatchGet(_ReturnAddress(), Context, Fact, FactKind::Int, result, ApplyInt, FormatInt, WriteInt);
		}

		float HookGetFloat(void *Context, void *Fact)
		{
			auto result = g_GetFloat(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Float);

				if (o)
				{
					if (o->m_WriteBack && result != o->m_Float)
					{
						g_SetFloat(Context, Fact, o->m_Float);
						LogWriteBack(Context, Fact, std::format("{:.3f}", o->m_Float));
					}

					result = o->m_Float;
				}

				Observe(*range, Context, Fact, FactKind::Float, false, o.has_value(), std::format("{:.3f}", result), result);
				return result;
			}

			return WatchGet(_ReturnAddress(), Context, Fact, FactKind::Float, result, ApplyFloat, FormatFloat, WriteFloat);
		}

		void *HookGetPosition(void *Out, void *Context, void *Fact)
		{
			auto result = g_GetPosition(Out, Context, Fact);
			auto position = static_cast<double *>(result ? result : Out);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Position);
				ApplyPositionOverride(position, o);

				Observe(
					*range,
					Context,
					Fact,
					FactKind::Position,
					false,
					o.has_value(),
					std::format("{:.0f}, {:.0f}, {:.0f}", position[0], position[1], position[2]),
					0,
					position);
				return result;
			}

			WatchGetPosition(_ReturnAddress(), Context, Fact, position);
			return result;
		}

		uint64_t HookSetBool(void *Context, void *Fact, bool Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Context, Fact, FactKind::Bool, true, false, Value ? "true" : "false", Value ? 1.0f : 0.0f);
			else
				WatchSet(_ReturnAddress(), Context, Fact, FactKind::Bool, FormatBool(Value), Value ? 1.0f : 0.0f);

			return g_SetBool(Context, Fact, Value);
		}

		uint64_t HookSetEnum(void *Context, void *Fact, void *Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Context, Fact, FactKind::Enum, true, false, EnumEntryName(Value));
			else
				WatchSet(_ReturnAddress(), Context, Fact, FactKind::Enum, EnumEntryName(Value));

			return g_SetEnum(Context, Fact, Value);
		}

		uint64_t HookSetInt(void *Context, void *Fact, int Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Context, Fact, FactKind::Int, true, false, std::to_string(Value), static_cast<float>(Value));
			else
				WatchSet(_ReturnAddress(), Context, Fact, FactKind::Int, std::to_string(Value), static_cast<float>(Value));

			return g_SetInt(Context, Fact, Value);
		}

		uint64_t HookSetFloat(void *Context, void *Fact, float Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Context, Fact, FactKind::Float, true, false, std::format("{:.3f}", Value), Value);
			else
				WatchSet(_ReturnAddress(), Context, Fact, FactKind::Float, std::format("{:.3f}", Value), Value);

			return g_SetFloat(Context, Fact, Value);
		}

		uint64_t HookSetPosition(void *Context, void *Fact, const void *Value)
		{
			const auto p = static_cast<const double *>(Value);
			const auto text = std::format("{:.0f}, {:.0f}, {:.0f}", p[0], p[1], p[2]);

			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Context, Fact, FactKind::Position, true, false, text, 0, p);
			else
				WatchSet(_ReturnAddress(), Context, Fact, FactKind::Position, text, 0, p);

			return g_SetPosition(Context, Fact, Value);
		}

		//
		// Native hooks: the exe's fact functions, for calls from game code. Only watched facts are looked at.
		//
		bool NativeGetBool(void *Context, void *Fact)
		{
			return WatchGet(_ReturnAddress(), Context, Fact, FactKind::Bool, g_GetBool(Context, Fact), ApplyBool, FormatBool, WriteBool);
		}

		void *NativeGetEnum(void *Context, void *Fact)
		{
			return WatchGetEnum(_ReturnAddress(), Context, Fact, g_GetEnum(Context, Fact));
		}

		int NativeGetInt(void *Context, void *Fact)
		{
			return WatchGet(_ReturnAddress(), Context, Fact, FactKind::Int, g_GetInt(Context, Fact), ApplyInt, FormatInt, WriteInt);
		}

		float NativeGetFloat(void *Context, void *Fact)
		{
			return WatchGet(_ReturnAddress(), Context, Fact, FactKind::Float, g_GetFloat(Context, Fact), ApplyFloat, FormatFloat, WriteFloat);
		}

		void *NativeGetPosition(void *Out, void *Context, void *Fact)
		{
			auto result = g_GetPosition(Out, Context, Fact);
			WatchGetPosition(_ReturnAddress(), Context, Fact, static_cast<double *>(result ? result : Out));
			return result;
		}

		uint64_t NativeSetBool(void *Context, void *Fact, bool Value)
		{
			WatchSet(_ReturnAddress(), Context, Fact, FactKind::Bool, FormatBool(Value), Value ? 1.0f : 0.0f);
			return g_SetBool(Context, Fact, Value);
		}

		uint64_t NativeSetEnum(void *Context, void *Fact, void *Value)
		{
			WatchSet(_ReturnAddress(), Context, Fact, FactKind::Enum, EnumEntryName(Value));
			return g_SetEnum(Context, Fact, Value);
		}

		uint64_t NativeSetInt(void *Context, void *Fact, int Value)
		{
			WatchSet(_ReturnAddress(), Context, Fact, FactKind::Int, std::to_string(Value), static_cast<float>(Value));
			return g_SetInt(Context, Fact, Value);
		}

		uint64_t NativeSetFloat(void *Context, void *Fact, float Value)
		{
			WatchSet(_ReturnAddress(), Context, Fact, FactKind::Float, std::format("{:.3f}", Value), Value);
			return g_SetFloat(Context, Fact, Value);
		}

		uint64_t NativeSetPosition(void *Context, void *Fact, const void *Value)
		{
			const auto p = static_cast<const double *>(Value);
			WatchSet(_ReturnAddress(), Context, Fact, FactKind::Position, std::format("{:.0f}, {:.0f}, {:.0f}", p[0], p[1], p[2]), 0, p);
			return g_SetPosition(Context, Fact, Value);
		}

		// Detours the exe functions behind the slots; the g_* pointers become trampolines used by every hook
		void InstallNativeHooks()
		{
			auto attach = [](auto **Original, auto *Hook)
			{
				DetourAttach(reinterpret_cast<void **>(Original), reinterpret_cast<void *>(Hook));
			};

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			attach(&g_GetBool, &NativeGetBool);
			attach(&g_GetEnum, &NativeGetEnum);
			attach(&g_GetInt, &NativeGetInt);
			attach(&g_GetFloat, &NativeGetFloat);
			attach(&g_GetPosition, &NativeGetPosition);
			attach(&g_SetBool, &NativeSetBool);
			attach(&g_SetEnum, &NativeSetEnum);
			attach(&g_SetInt, &NativeSetInt);
			attach(&g_SetFloat, &NativeSetFloat);
			attach(&g_SetPosition, &NativeSetPosition);

			if (const auto error = DetourTransactionCommit(); error != NO_ERROR)
			{
				spdlog::error("[graph] native fact hooks failed ({})", error);
				return;
			}

			g_NativeInstalled = true;
			spdlog::info("[graph] native fact hooks installed");
		}

		bool IsInsideModule(void *Address, HMODULE Module)
		{
			const auto base = reinterpret_cast<uintptr_t>(Module);
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
			const auto address = reinterpret_cast<uintptr_t>(Address);

			return address >= base && address < base + nt->OptionalHeader.SizeOfImage;
		}

		template<typename T>
		bool SwapSlot(uintptr_t Rva, T *Hook, T **Original)
		{
			auto slot = reinterpret_cast<T **>(g_Base + Rva);

			if (!*slot || !IsInsideModule(reinterpret_cast<void *>(*slot), GetModuleHandleW(nullptr)))
				return false;

			*Original = *slot;
			*slot = Hook;
			return true;
		}

		// Slot probes: log what other native API functions return to watched graphs, optionally forcing the result
		using ProbeFn = uint64_t (*)(void *, void *, void *, void *);

		struct ProbeState
		{
			uintptr_t m_Rva = 0;
			ProbeFn m_Original = nullptr;
			std::vector<std::string> m_Graphs;
			std::optional<uint64_t> m_Force;
			bool m_DescribeEntity = false;
			bool m_DumpResult = false;
		};

		struct ComponentInfo
		{
			uint16_t m_Type;
			const RTTI *m_RTTI;
			RTTIRefObject *m_Resource;
		};

		// Reads an entity's components without trusting the pointer; no C++ objects with destructors here (SEH)
		size_t ReadEntityComponents(void *Object, const RTTI **EntityType, RTTIRefObject **EntityResource, ComponentInfo *Out, size_t Max)
		{
			__try
			{
				auto entity = static_cast<Entity *>(Object);
				*EntityType = entity->GetRTTI();
				*EntityResource = entity->m_EntityResource.GetUntypedPtr();

				const auto& components = entity->m_Components.m_Components;
				const auto& types = entity->m_Components.m_ComponentTypes;
				const size_t count = std::min<size_t>(components.size(), Max);

				for (size_t i = 0; i < count; i++)
				{
					auto component = components.data()[i];
					Out[i].m_Type = i < types.size() ? types.data()[i] : 0xFFFF;
					Out[i].m_RTTI = component ? component->GetRTTI() : nullptr;
					Out[i].m_Resource = component ? component->m_Resource.GetPtr() : nullptr;
				}

				return count;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return SIZE_MAX;
			}
		}

		const RTTI *SafeGetRTTI(void *Object)
		{
			__try
			{
				return static_cast<RTTIRefObject *>(Object)->GetRTTI();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		RTTIRefObject *SafeComponentResource(void *Object)
		{
			__try
			{
				return static_cast<EntityComponent *>(Object)->m_Resource.GetPtr();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		std::string ResourceLabel(RTTIRefObject *Resource)
		{
			if (!Resource)
				return "-";

			const auto rtti = SafeGetRTTI(Resource);
			return std::format("{} {}", rtti ? rtti->GetSymbolName() : "?", Resource->m_UUID);
		}

		void DescribeEntity(void *Object)
		{
			std::array<ComponentInfo, 128> components {};
			const RTTI *type = nullptr;
			RTTIRefObject *resource = nullptr;
			const auto count = ReadEntityComponents(Object, &type, &resource, components.data(), components.size());

			if (count == SIZE_MAX || !type)
			{
				spdlog::info("[probe] {} is not a readable entity", Object);
				return;
			}

			spdlog::info("[probe] entity {} {} resource {}: {} components", Object, type->GetSymbolName(), ResourceLabel(resource), count);

			for (size_t i = 0; i < count; i++)
			{
				const auto& c = components[i];
				spdlog::info(
					"[probe]   [{}] type {} {} resource {}",
					i,
					c.m_Type,
					c.m_RTTI ? c.m_RTTI->GetSymbolName() : "null",
					ResourceLabel(c.m_Resource));
			}
		}

		void DumpResultObject(uint64_t Result)
		{
			const auto object = reinterpret_cast<RTTIRefObject *>(Result);
			const auto rtti = SafeGetRTTI(object);

			if (!rtti)
			{
				spdlog::info("[probe] result {:#x} has no RTTI", Result);
				return;
			}

			spdlog::info("[probe] result {:#x} is {}", Result, rtti->GetSymbolName());

			// A component: its resource holds the data; otherwise dump the object itself
			const auto name = rtti->GetSymbolName();

			if (name.find("Resource") == std::string::npos)
			{
				if (auto resource = SafeComponentResource(object); resource && SafeGetRTTI(resource))
				{
					ModCoreEvents::GetInstance().DumpObject(resource, "probe result resource");
					spdlog::info("[probe] dumped {} to asset_dump.txt", ResourceLabel(resource));
					return;
				}
			}

			ModCoreEvents::GetInstance().DumpObject(object, "probe result");
			spdlog::info("[probe] dumped the result to asset_dump.txt");
		}

		std::mutex g_DescribedMutex;
		std::unordered_set<uintptr_t> g_Described;

		constexpr size_t MaxProbes = 8;
		std::array<ProbeState, MaxProbes> g_Probes;
		std::mutex g_ProbeMutex;
		std::map<std::tuple<size_t, size_t, uint64_t, uint64_t>, uint64_t> g_ProbeLogged;

		template<size_t N>
		uint64_t ProbeHook(void *A, void *B, void *C, void *D)
		{
			const auto& probe = g_Probes[N];
			auto result = probe.m_Original(A, B, C, D);
			const auto range = FindRange(_ReturnAddress());

			if (!range || !TagMatches(g_Tags[range->m_Tag], probe.m_Graphs))
				return result;

			const auto original = result;

			if (probe.m_DescribeEntity || probe.m_DumpResult)
			{
				bool describe = false;
				bool dump = false;
				{
					std::scoped_lock lock(g_DescribedMutex);
					describe = probe.m_DescribeEntity && g_Described.insert(reinterpret_cast<uintptr_t>(A)).second;
					dump = probe.m_DumpResult && original && g_Described.insert(original | 1).second;
				}

				if (describe)
					DescribeEntity(A);

				if (dump)
					DumpResultObject(original);
			}

			if (probe.m_Force)
				result = *probe.m_Force;

			const auto context = ContextId(A);
			const auto second = reinterpret_cast<uint64_t>(B);

			std::scoped_lock lock(g_ProbeMutex);
			auto [itr, inserted] = g_ProbeLogged.try_emplace(std::make_tuple(N, range->m_Tag, context, second), original);

			if (inserted || itr->second != original)
			{
				itr->second = original;
				spdlog::info(
					"[probe] {:#x} from {}: a={:#x} ctx={:#x} b={:#x} -> {:#x}{}",
					probe.m_Rva,
					g_Tags[range->m_Tag],
					reinterpret_cast<uint64_t>(A),
					context,
					second,
					original,
					probe.m_Force ? std::format(" (forced {:#x})", result) : "");
			}

			return result;
		}

		template<size_t... I>
		constexpr auto MakeProbeTable(std::index_sequence<I...>)
		{
			return std::array<ProbeFn, sizeof...(I)> { &ProbeHook<I>... };
		}

		const auto ProbeHooks = MakeProbeTable(std::make_index_sequence<MaxProbes>());

		void InstallProbes()
		{
			size_t used = 0;

			for (const auto& config : ModConfiguration.SlotProbes)
			{
				if (!config.Enabled)
					continue;

				if (used >= MaxProbes)
				{
					spdlog::error("[probe] at most {} slot probes", MaxProbes);
					break;
				}

				uintptr_t rva = 0;
				const bool hex = config.Slot.starts_with("0x");

				if (std::from_chars(config.Slot.data() + (hex ? 2 : 0), config.Slot.data() + config.Slot.size(), rva, hex ? 16 : 10).ec !=
						std::errc() ||
					rva % 8 != 0)
				{
					spdlog::error("[probe] invalid slot '{}'", config.Slot);
					continue;
				}

				auto& probe = g_Probes[used];
				probe.m_Rva = rva;
				probe.m_Graphs = config.Graphs;
				probe.m_DescribeEntity = config.DescribeEntity;
				probe.m_DumpResult = config.DumpResult;

				if (!config.Force.empty())
				{
					if (config.Force == "true" || config.Force == "false")
						probe.m_Force = config.Force == "true" ? 1 : 0;
					else
					{
						uint64_t value = 0;
						const bool forceHex = config.Force.starts_with("0x");

						if (std::from_chars(config.Force.data() + (forceHex ? 2 : 0), config.Force.data() + config.Force.size(), value, forceHex ? 16 : 10)
								.ec == std::errc())
							probe.m_Force = value;
					}
				}

				if (!SwapSlot(rva, reinterpret_cast<std::remove_pointer_t<ProbeFn> *>(ProbeHooks[used]), &probe.m_Original))
				{
					spdlog::error("[probe] slot {:#x} is empty or not native code", rva);
					continue;
				}

				spdlog::info("[probe] slot {:#x} hooked{}", rva, probe.m_Force ? std::format(", forcing {:#x}", *probe.m_Force) : "");
				used++;
			}
		}

		void CollectExports(const ExportEntry *Table, uint32_t Count, void *)
		{
			g_Exports.reserve(Count);
			g_ExportAddresses.reserve(Count);

			for (uint32_t i = 0; i < Count; i++)
			{
				g_Exports.emplace(std::array<uint64_t, 2> { Table[i].m_Hash[0], Table[i].m_Hash[1] }, Table[i].m_Function);
				g_ExportAddresses.emplace_back(reinterpret_cast<uintptr_t>(Table[i].m_Function));
			}

			std::ranges::sort(g_ExportAddresses);
		}

		// Caller holds g_Mutex
		void RegisterGraphLocked(RTTIRefObject *Graph)
		{
			const auto tagCount = g_TagCount.load();
			auto program = static_cast<ProgramResource *>(Graph);

			for (size_t t = 0; t < tagCount; t++)
			{
				uintptr_t begin = UINTPTR_MAX;
				uintptr_t last = 0;
				size_t entries = 0;

				for (const auto& entry : program->m_EntryPoints)
				{
					const std::string_view name(entry.m_EntryPoint.c_str());

					if (name.find(g_Tags[t]) == std::string_view::npos)
						continue;

					if (auto itr = g_Exports.find(Murmur3(name, 42)); itr != g_Exports.end())
					{
						const auto address = reinterpret_cast<uintptr_t>(itr->second);
						begin = std::min(begin, address);
						last = std::max(last, address);
						entries++;
					}
				}

				if (!entries)
					continue;

				// Helper functions are emitted between a graph's entry points and the next exported function
				auto next = std::upper_bound(g_ExportAddresses.begin(), g_ExportAddresses.end(), last);
				const auto end = next != g_ExportAddresses.end() ? *next : last + 0x1000;
				const auto count = g_RangeCount.load();

				const bool known = std::any_of(g_Ranges.begin(), g_Ranges.begin() + count, [&](const auto& r) { return r.m_Begin == begin; });

				if (known || count >= MaxRanges)
					continue;

				g_Ranges[count] = { begin, end, t };
				g_RangeCount.store(count + 1, std::memory_order_release);
				g_TagEntryCount[t] += entries;

				spdlog::info("[graph] watching '{}' at {:#x}-{:#x} ({} entry points)", g_Tags[t], begin - g_Base, end - g_Base, entries);

				for (const auto& entry : program->m_EntryPoints)
				{
					const std::string_view name(entry.m_EntryPoint.c_str());

					if (name.find(g_Tags[t]) != std::string_view::npos && g_Exports.contains(Murmur3(name, 42)))
						spdlog::info("[graph]   entry point {}", name);
				}
			}
		}

		void RescanCachedGraphsLocked()
		{
			auto& events = ModCoreEvents::GetInstance();
			std::shared_lock lock(events.m_CachedDataMutex);

			for (auto graph : events.m_CachedGraphPrograms)
				RegisterGraphLocked(graph);
		}
	}

	void TryInstall()
	{
		if (g_Installed.load() || (g_TagCount.load() == 0 && g_WatchedFactCount.load() == 0))
			return;

		const auto module = GetModuleHandleW(L"fullgame.dll");

		if (!module)
			return;

		g_Base = reinterpret_cast<uintptr_t>(module);

		{
			const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(exe + reinterpret_cast<const IMAGE_DOS_HEADER *>(exe)->e_lfanew);
			g_ExeBase = exe;
			g_ExeEnd = exe + nt->OptionalHeader.SizeOfImage;
		}

		// The exe fills the native API slots after the library loads
		if (!*reinterpret_cast<void **>(g_Base + SlotGetBool))
			return;

		std::scoped_lock lock(g_Mutex);

		if (g_Installed.load())
			return;

		const auto cacheExports = reinterpret_cast<void (*)(void (*)(const ExportEntry *, uint32_t, void *), void *)>(
			GetProcAddress(module, "CacheLibraryExports"));

		if (!cacheExports)
		{
			spdlog::error("[graph] fullgame.dll has no CacheLibraryExports, graph hooks disabled");
			g_Installed = true;
			return;
		}

		cacheExports(&CollectExports, nullptr);

		const bool ok = SwapSlot(SlotGetBool, &HookGetBool, &g_GetBool) && SwapSlot(SlotGetEnum, &HookGetEnum, &g_GetEnum) &&
						SwapSlot(SlotGetInt, &HookGetInt, &g_GetInt) && SwapSlot(SlotGetFloat, &HookGetFloat, &g_GetFloat) &&
						SwapSlot(SlotGetPosition, &HookGetPosition, &g_GetPosition) && SwapSlot(SlotSetBool, &HookSetBool, &g_SetBool) &&
						SwapSlot(SlotSetEnum, &HookSetEnum, &g_SetEnum) && SwapSlot(SlotSetInt, &HookSetInt, &g_SetInt) &&
						SwapSlot(SlotSetFloat, &HookSetFloat, &g_SetFloat) &&
						SwapSlot(SlotSetPosition, &HookSetPosition, &g_SetPosition);

		g_Installed = true;

		if (!ok)
		{
			spdlog::error("[graph] fact API slots do not match this game build, fact hooks are incomplete");
			return;
		}

		spdlog::info("[graph] installed: {} exported graph functions", g_Exports.size());
		InstallProbes();
		RescanCachedGraphsLocked();

		if (ModConfiguration.NativeFactHooks)
			InstallNativeHooks();
	}

	bool IsInstalled()
	{
		return g_Installed.load() && g_GetBool;
	}

	void OnGraphProgramLoaded(RTTIRefObject *Graph)
	{
		if (!g_Installed.load())
			return;

		std::scoped_lock lock(g_Mutex);
		RegisterGraphLocked(Graph);
	}

	void WatchGraph(const std::string& Tag, bool Trace)
	{
		if (Tag.empty())
			return;

		{
			std::scoped_lock lock(g_Mutex);
			const auto count = g_TagCount.load();

			for (size_t i = 0; i < count; i++)
			{
				if (g_Tags[i] == Tag)
				{
					g_TagTrace[i] = g_TagTrace[i] || Trace;
					return;
				}
			}

			if (count >= MaxTags)
				return;

			g_Tags[count] = Tag;
			g_TagTrace[count] = Trace;
			g_TagCount.store(count + 1);

			if (g_Installed.load())
				RescanCachedGraphsLocked();
		}

		TryInstall();
	}

	void SetTrace(const std::string& Tag, bool Trace)
	{
		std::scoped_lock lock(g_Mutex);

		for (size_t i = 0; i < g_TagCount.load(); i++)
		{
			if (g_Tags[i] == Tag)
				g_TagTrace[i] = Trace;
		}
	}

	std::vector<WatchedGraph> GetWatchedGraphs()
	{
		std::scoped_lock lock(g_Mutex);
		std::vector<WatchedGraph> result;

		for (size_t i = 0; i < g_TagCount.load(); i++)
		{
			WatchedGraph g { g_Tags[i], g_TagTrace[i].load(), g_TagEntryCount[i], 0, 0 };

			for (size_t r = 0; r < g_RangeCount.load(); r++)
			{
				if (g_Ranges[r].m_Tag == i && !g.m_Begin)
				{
					g.m_Begin = g_Ranges[r].m_Begin - g_Base;
					g.m_End = g_Ranges[r].m_End - g_Base;
				}
			}

			result.emplace_back(std::move(g));
		}

		return result;
	}

	void *FindEntryPoint(std::string_view Name)
	{
		std::scoped_lock lock(g_Mutex);

		if (auto itr = g_Exports.find(Murmur3(Name, 42)); itr != g_Exports.end())
			return itr->second;

		return nullptr;
	}

	void AddOverride(FactOverride Override)
	{
		std::unique_lock lock(g_OverrideMutex);
		g_Overrides.emplace_back(std::move(Override));
	}

	void SetOverrides(const std::string& Source, std::vector<FactOverride> Overrides)
	{
		std::unique_lock lock(g_OverrideMutex);
		std::erase_if(g_Overrides, [&](const auto& o) { return o.m_Source == Source; });

		for (auto& o : Overrides)
		{
			o.m_Source = Source;
			g_Overrides.emplace_back(std::move(o));
		}
	}

	void RemoveOverrides(const std::string& Source)
	{
		std::unique_lock lock(g_OverrideMutex);
		std::erase_if(g_Overrides, [&](const auto& o) { return o.m_Source == Source; });
	}

	void SetOverrideEnabled(size_t Index, bool Enabled)
	{
		std::unique_lock lock(g_OverrideMutex);

		if (Index < g_Overrides.size())
			g_Overrides[Index].m_Enabled = Enabled;
	}

	void RemoveOverride(size_t Index)
	{
		std::unique_lock lock(g_OverrideMutex);

		if (Index < g_Overrides.size())
			g_Overrides.erase(g_Overrides.begin() + Index);
	}

	std::vector<FactOverride> GetOverrides()
	{
		std::shared_lock lock(g_OverrideMutex);
		return g_Overrides;
	}

	std::string OverrideToToml(const FactOverride& Override)
	{
		std::string graphs;

		for (const auto& g : Override.m_Graphs)
			graphs += std::format("{}\"{}\"", graphs.empty() ? "" : ", ", g);

		std::string value;
		const auto& v = Override.m_Value;

		switch (v.m_Kind)
		{
		case FactKind::Bool:
			value = v.m_Bool ? "true" : "false";
			break;
		case FactKind::Enum:
			value = v.m_EnumEntry ? std::format("{}", *v.m_EnumEntry) : "default";
			break;
		case FactKind::Int:
			value = std::to_string(v.m_Int);
			break;
		case FactKind::Float:
			value = std::format("{}", v.m_Float);
			break;
		case FactKind::Position:
			for (int i = 0; i < 3; i++)
				value += std::format("{}{}", i ? "," : "", v.m_PositionAxes[i] ? std::format("{}", v.m_Position[i]) : "*");
			break;
		}

		return std::format(
			"[[FactOverride]]\nGraphs = [{}]\nFact = \"{}\"  # {}\nType = \"{}\"\nValue = \"{}\"\nWriteBack = {}\n",
			graphs,
			Override.m_Fact,
			GetFactLabel(Override.m_Fact),
			KindName(v.m_Kind),
			value,
			v.m_WriteBack);
	}

	void WatchFact(const GGUUID& Fact)
	{
		{
			std::scoped_lock lock(g_Mutex);
			const auto count = g_WatchedFactCount.load();

			if (count >= MaxWatchedFacts || std::find(g_WatchedFacts.begin(), g_WatchedFacts.begin() + count, Fact) != g_WatchedFacts.begin() + count)
				return;

			g_WatchedFacts[count] = Fact;
			g_WatchedFactCount.store(count + 1, std::memory_order_release);
		}

		TryInstall();
	}

	void UnwatchFact(const GGUUID& Fact)
	{
		std::scoped_lock lock(g_Mutex);
		const auto count = g_WatchedFactCount.load();
		std::vector<GGUUID> keep;

		for (size_t i = 0; i < count; i++)
		{
			if (g_WatchedFacts[i] != Fact)
				keep.emplace_back(g_WatchedFacts[i]);
		}

		// Shrink first so lock-free readers never see a stale tail
		g_WatchedFactCount.store(0, std::memory_order_release);
		std::copy(keep.begin(), keep.end(), g_WatchedFacts.begin());
		g_WatchedFactCount.store(keep.size(), std::memory_order_release);
	}

	std::vector<GGUUID> GetWatchedFacts()
	{
		std::scoped_lock lock(g_Mutex);
		return { g_WatchedFacts.begin(), g_WatchedFacts.begin() + g_WatchedFactCount.load() };
	}

	bool NativeHooksInstalled()
	{
		return g_NativeInstalled;
	}

	std::vector<ObservedFact> GetObservedFacts()
	{
		std::scoped_lock lock(g_ObservedMutex);
		std::vector<ObservedFact> result;
		result.reserve(g_Observed.size() + g_WatchObserved.size());

		for (const auto& [key, entry] : g_Observed)
			result.emplace_back(entry);

		for (const auto& [key, entry] : g_WatchObserved)
			result.emplace_back(entry);

		return result;
	}

	std::optional<ObservedFact> GetObservedFact(const GGUUID& Fact)
	{
		std::scoped_lock lock(g_ObservedMutex);
		const ObservedEntry *latest = nullptr;

		for (const auto& [key, entry] : g_Observed)
		{
			if (entry.m_Fact == Fact && (!latest || entry.m_LastTick > latest->m_LastTick))
				latest = &entry;
		}

		for (const auto& [key, entry] : g_WatchObserved)
		{
			if (entry.m_Fact == Fact && (!latest || entry.m_LastTick > latest->m_LastTick))
				latest = &entry;
		}

		if (!latest)
			return std::nullopt;

		return *latest;
	}

	void ClearObservedFacts()
	{
		std::scoped_lock lock(g_ObservedMutex);
		g_Observed.clear();
		g_WatchObserved.clear();
	}

	void SetFactLabel(const GGUUID& Fact, std::string Label)
	{
		std::scoped_lock lock(g_LabelMutex);
		g_Labels[std::format("{}", Fact)] = std::move(Label);
	}

	std::string GetFactLabel(const GGUUID& Fact)
	{
		const auto key = std::format("{}", Fact);
		std::scoped_lock lock(g_LabelMutex);

		if (auto itr = g_Labels.find(key); itr != g_Labels.end())
			return std::format("{} ({})", itr->second, key);

		return key;
	}

	const char *KindName(FactKind Kind)
	{
		switch (Kind)
		{
		case FactKind::Bool:
			return "Bool";
		case FactKind::Enum:
			return "Enum";
		case FactKind::Int:
			return "Int";
		case FactKind::Float:
			return "Float";
		case FactKind::Position:
			return "Position";
		}

		return "?";
	}

	std::optional<OverrideValue> ParseOverrideValue(FactKind Kind, std::string_view Text)
	{
		OverrideValue v;
		v.m_Kind = Kind;

		const auto parseNumber = [](std::string_view S, auto& Out)
		{
			const auto first = S.find_first_not_of(" \t");
			const auto last = S.find_last_not_of(" \t");

			if (first == std::string_view::npos)
				return false;

			S = S.substr(first, last - first + 1);
			return std::from_chars(S.data(), S.data() + S.size(), Out).ec == std::errc {};
		};

		switch (Kind)
		{
		case FactKind::Bool:
		{
			const auto t = Lower(Text);

			if (t != "true" && t != "false" && t != "1" && t != "0")
				return std::nullopt;

			v.m_Bool = t == "true" || t == "1";
			return v;
		}

		case FactKind::Enum:
			if (Lower(Text) == "default")
				return v;

			if (auto uuid = GGUUID::TryParse(Text))
			{
				v.m_EnumEntry = uuid;
				return v;
			}

			return std::nullopt;

		case FactKind::Int:
			return parseNumber(Text, v.m_Int) ? std::optional(v) : std::nullopt;

		case FactKind::Float:
			return parseNumber(Text, v.m_Float) ? std::optional(v) : std::nullopt;

		case FactKind::Position:
		{
			size_t start = 0;

			for (int i = 0; i < 3; i++)
			{
				const auto comma = Text.find(',', start);
				const auto part = Text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);

				if (part.find('*') != std::string_view::npos)
					v.m_PositionAxes[i] = false;
				else if (!parseNumber(part, v.m_Position[i]))
					return std::nullopt;

				if (comma == std::string_view::npos && i < 2)
					return std::nullopt;

				start = comma + 1;
			}

			return v;
		}
		}

		return std::nullopt;
	}

	void LoadConfig()
	{
		SetFactLabel(GGUUID::Parse("B44A1CCD-7CC4-7A4E-A097-AB111E33552E"), "Is local called mount");
		SetFactLabel(GGUUID::Parse("A6FA2D8F-4ED3-DC4E-AF22-007FD821270B"), "Is overridden (hacked)");
		SetFactLabel(GGUUID::Parse("0A94E442-C4B4-2749-89BC-41A5170F4BD1"), "Overridden machine behavior mode");
		SetFactLabel(GGUUID::Parse("1A73D727-5F54-244B-A8F9-3E18000BE8F4"), "Player override mode selection");
		SetFactLabel(GGUUID::Parse("15819EB0-5AC2-A748-91E6-806C9273046E"), "Override target machine type");

		// The hacking start script forces protective mode on Sunwings and Waterwings; let it see an untyped machine
		if (ModConfiguration.AllowAggressiveFlyingMountOverride)
		{
			FactOverride o;
			o.m_Graphs = { "HackingStartGraph_c4b070d94cecc0d8d07dc31bc9538e50" };
			o.m_Fact = GGUUID::Parse("15819EB0-5AC2-A748-91E6-806C9273046E");
			o.m_Value.m_Kind = FactKind::Enum;
			o.m_Source = "flying-mount-aggressive";

			WatchGraph(o.m_Graphs[0], false);
			SetOverrides(o.m_Source, { o });
		}

		for (const auto& tag : ModConfiguration.WatchGraphs)
			WatchGraph(tag, false);

		for (const auto& fact : ModConfiguration.WatchFacts)
		{
			if (auto uuid = GGUUID::TryParse(fact))
				WatchFact(*uuid);
			else
				spdlog::error("[graph] invalid WatchFacts entry '{}'", fact);
		}

		for (const auto& tag : ModConfiguration.TraceGraphs)
		{
			WatchGraph(tag, true);
			SetTrace(tag, true);
		}

		std::vector<FactOverride> overrides;

		for (const auto& entry : ModConfiguration.FactOverrides)
		{
			std::string type(entry.Type);
			std::ranges::transform(type, type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			std::optional<FactKind> kind;

			for (auto k : { FactKind::Bool, FactKind::Enum, FactKind::Int, FactKind::Float, FactKind::Position })
			{
				std::string name(KindName(k));
				std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

				if (name == type)
					kind = k;
			}

			const auto fact = GGUUID::TryParse(entry.Fact);
			const auto value = kind ? ParseOverrideValue(*kind, entry.Value) : std::nullopt;

			if (!fact || !value)
			{
				spdlog::error("[graph] invalid [[FactOverride]] Fact='{}' Type='{}' Value='{}'", entry.Fact, entry.Type, entry.Value);
				continue;
			}

			FactOverride o;
			o.m_Graphs = entry.Graphs;
			o.m_Fact = *fact;
			o.m_Value = *value;
			o.m_Enabled = entry.Enabled;
			o.m_Value.m_WriteBack = entry.WriteBack;
			o.m_Source = "config";

			// Graph tags named by an override must be watched for it to apply; "native" needs the fact watched instead
			for (const auto& tag : o.m_Graphs)
			{
				if (Lower(tag).starts_with("native"))
					WatchFact(o.m_Fact);
				else
					WatchGraph(tag, false);
			}

			overrides.emplace_back(std::move(o));
		}

		if (!overrides.empty())
		{
			spdlog::info("[graph] loaded {} fact overrides from config", overrides.size());
			SetOverrides("config", std::move(overrides));
		}
	}

	std::optional<bool> ReadBool(void *Context, void *Fact)
	{
		if (!IsInstalled() || !Fact || !g_GetBool)
			return std::nullopt;

		return g_GetBool(Context, Fact);
	}

	void *ReadEnum(void *Context, void *Fact)
	{
		if (!IsInstalled() || !Fact || !g_GetEnum)
			return nullptr;

		return g_GetEnum(Context, Fact);
	}

	std::optional<int> ReadInt(void *Context, void *Fact)
	{
		if (!IsInstalled() || !Fact || !g_GetInt)
			return std::nullopt;

		return g_GetInt(Context, Fact);
	}

	std::optional<float> ReadFloat(void *Context, void *Fact)
	{
		if (!IsInstalled() || !Fact || !g_GetFloat)
			return std::nullopt;

		return g_GetFloat(Context, Fact);
	}

	bool ReadPosition(void *Context, void *Fact, double Out[3])
	{
		if (!IsInstalled() || !Fact || !g_GetPosition)
			return false;

		alignas(16) double buffer[4] = {};
		auto result = static_cast<const double *>(g_GetPosition(buffer, Context, Fact));
		std::copy_n(result ? result : buffer, 3, Out);
		return true;
	}

	bool WriteBool(void *Context, void *Fact, bool Value)
	{
		if (!IsInstalled() || !Fact || !g_SetBool)
			return false;

		g_SetBool(Context, Fact, Value);
		return true;
	}

	bool WriteInt(void *Context, void *Fact, int Value)
	{
		if (!IsInstalled() || !Fact || !g_SetInt)
			return false;

		g_SetInt(Context, Fact, Value);
		return true;
	}

	bool WriteFloat(void *Context, void *Fact, float Value)
	{
		if (!IsInstalled() || !Fact || !g_SetFloat)
			return false;

		g_SetFloat(Context, Fact, Value);
		return true;
	}

	bool WritePosition(void *Context, void *Fact, const double Value[3])
	{
		if (!IsInstalled() || !Fact || !g_SetPosition)
			return false;

		alignas(16) double buffer[4] = { Value[0], Value[1], Value[2], 0.0 };
		g_SetPosition(Context, Fact, buffer);
		return true;
	}

	bool GetObjectUUID(const void *Object, GGUUID *Out)
	{
		return Object && SafeReadUUID(Object, Out);
	}

	std::string DescribeEnumEntry(void *Entry)
	{
		return EnumEntryName(Entry);
	}
}
