#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include "../ModConfiguration.h"
#include "../ModCoreEvents.h"
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

		std::mutex g_Mutex;
		std::atomic_bool g_Installed = false;
		uintptr_t g_Base = 0;
		std::unordered_map<std::array<uint64_t, 2>, void *, HashKey> g_Exports;
		std::vector<uintptr_t> g_ExportAddresses;

		std::array<std::string, MaxTags> g_Tags;
		std::atomic_size_t g_TagCount = 0;

		std::array<CodeRange, MaxRanges> g_Ranges = {};
		std::atomic_size_t g_RangeCount = 0;

		std::shared_mutex g_OverrideMutex;
		std::vector<FactOverride> g_Overrides;

		std::mutex g_ObservedMutex;
		std::map<std::pair<size_t, GGUUID>, ObservedFact> g_Observed;

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

		void Observe(const CodeRange& Range, void *Fact, FactKind Kind, bool Overridden, std::string Value, float Number = 0, const double *Position = nullptr)
		{
			GGUUID uuid {};

			if (!SafeReadUUID(Fact, &uuid))
				return;

			std::scoped_lock lock(g_ObservedMutex);
			auto& entry = g_Observed[std::make_pair(Range.m_Tag, uuid)];

			if (entry.m_Count == 0)
			{
				entry.m_Graph = g_Tags[Range.m_Tag];
				entry.m_Fact = uuid;
				entry.m_Kind = Kind;
			}

			entry.m_Overridden = Overridden;
			entry.m_Number = Number;
			entry.m_Count++;
			entry.m_LastTick = GetTickCount64();

			if (Position)
				std::copy_n(Position, 3, entry.m_Position);

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
					result = o->m_Bool;

				Observe(*range, Fact, FactKind::Bool, o.has_value(), result ? "true" : "false", result ? 1.0f : 0.0f);
			}

			return result;
		}

		void *HookGetEnum(void *Context, void *Fact)
		{
			auto result = g_GetEnum(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				auto value = ApplyEnumOverride(Fact, result, LookupOverride(*range, Fact, FactKind::Enum));
				Observe(*range, Fact, FactKind::Enum, value != result, EnumEntryName(value));
				return value;
			}

			return result;
		}

		int HookGetInt(void *Context, void *Fact)
		{
			auto result = g_GetInt(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Int);

				if (o)
					result = o->m_Int;

				Observe(*range, Fact, FactKind::Int, o.has_value(), std::to_string(result), static_cast<float>(result));
			}

			return result;
		}

		float HookGetFloat(void *Context, void *Fact)
		{
			auto result = g_GetFloat(Context, Fact);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Float);

				if (o)
					result = o->m_Float;

				Observe(*range, Fact, FactKind::Float, o.has_value(), std::format("{:.3f}", result), result);
			}

			return result;
		}

		void *HookGetPosition(void *Out, void *Context, void *Fact)
		{
			auto result = g_GetPosition(Out, Context, Fact);
			auto position = static_cast<double *>(result ? result : Out);

			if (auto range = FindRange(_ReturnAddress()))
			{
				const auto o = LookupOverride(*range, Fact, FactKind::Position);
				ApplyPositionOverride(position, o);
				Observe(*range, Fact, FactKind::Position, o.has_value(), std::format("{:.0f}, {:.0f}, {:.0f}", position[0], position[1], position[2]), 0, position);
			}

			return result;
		}

		uint64_t HookSetBool(void *Context, void *Fact, bool Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Fact, FactKind::Bool, false, Value ? "true" : "false", Value ? 1.0f : 0.0f);

			return g_SetBool(Context, Fact, Value);
		}

		uint64_t HookSetEnum(void *Context, void *Fact, void *Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Fact, FactKind::Enum, false, EnumEntryName(Value));

			return g_SetEnum(Context, Fact, Value);
		}

		uint64_t HookSetInt(void *Context, void *Fact, int Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Fact, FactKind::Int, false, std::to_string(Value), static_cast<float>(Value));

			return g_SetInt(Context, Fact, Value);
		}

		uint64_t HookSetFloat(void *Context, void *Fact, float Value)
		{
			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Fact, FactKind::Float, false, std::format("{:.3f}", Value), Value);

			return g_SetFloat(Context, Fact, Value);
		}

		uint64_t HookSetPosition(void *Context, void *Fact, const void *Value)
		{
			const auto p = static_cast<const double *>(Value);

			if (auto range = FindRange(_ReturnAddress()))
				Observe(*range, Fact, FactKind::Position, false, std::format("{:.0f}, {:.0f}, {:.0f}", p[0], p[1], p[2]), 0, p);

			return g_SetPosition(Context, Fact, Value);
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

				spdlog::info("[graph] watching '{}' at {:#x}-{:#x} ({} entry points)", g_Tags[t], begin - g_Base, end - g_Base, entries);
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
		if (g_Installed.load() || g_TagCount.load() == 0)
			return;

		const auto module = GetModuleHandleW(L"fullgame.dll");

		if (!module)
			return;

		g_Base = reinterpret_cast<uintptr_t>(module);

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
		RescanCachedGraphsLocked();
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

	void WatchGraph(const std::string& Tag)
	{
		if (Tag.empty())
			return;

		{
			std::scoped_lock lock(g_Mutex);
			const auto count = g_TagCount.load();

			for (size_t i = 0; i < count; i++)
			{
				if (g_Tags[i] == Tag)
					return;
			}

			if (count >= MaxTags)
				return;

			g_Tags[count] = Tag;
			g_TagCount.store(count + 1);

			if (g_Installed.load())
				RescanCachedGraphsLocked();
		}

		TryInstall();
	}

	void *FindEntryPoint(std::string_view Name)
	{
		std::scoped_lock lock(g_Mutex);

		if (auto itr = g_Exports.find(Murmur3(Name, 42)); itr != g_Exports.end())
			return itr->second;

		return nullptr;
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

	std::optional<ObservedFact> GetObservedFact(const GGUUID& Fact)
	{
		std::scoped_lock lock(g_ObservedMutex);
		const ObservedFact *latest = nullptr;

		for (const auto& [key, entry] : g_Observed)
		{
			if (entry.m_Fact == Fact && (!latest || entry.m_LastTick > latest->m_LastTick))
				latest = &entry;
		}

		if (!latest)
			return std::nullopt;

		return *latest;
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
		// The hacking start script forces protective mode on Sunwings and Waterwings; let it see an untyped machine
		if (ModConfiguration.AllowAggressiveFlyingMountOverride)
		{
			FactOverride o;
			o.m_Graphs = { "HackingStartGraph_c4b070d94cecc0d8d07dc31bc9538e50" };
			o.m_Fact = GGUUID::Parse("15819EB0-5AC2-A748-91E6-806C9273046E");
			o.m_Value.m_Kind = FactKind::Enum;

			WatchGraph(o.m_Graphs[0]);
			SetOverrides("flying-mount-aggressive", { o });
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
}
