#include <Windows.h>
#include <detours/detours.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <optional>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "ModConfiguration.h"
#include "Htn.h"

namespace Htn
{
	namespace
	{
		// Build HRZ2-PCR 80/9966125. RVAs in fullgame.dll, found from each domain's init function.
		struct DomainInfo
		{
			const char *m_Name;
			uintptr_t m_Dispatch;  // (pool, ?, ?, task cell, arity) -> compiled task
			uintptr_t m_Behave;	   // Root task "behave": (planner, world state, frame)
			uintptr_t m_Names;	   // char *[] symbol names
			uint32_t m_NameCount;
			uintptr_t m_SymbolIndices; // uint16 name index per symbol cell
			uintptr_t m_SymbolDeltas;  // uint8 delta (in qwords) to the next symbol cell
			uint32_t m_SymbolCells;
			uintptr_t m_Predicates;	   // char *[] world state predicate names, indexed by predicate number
			uint32_t m_PredicateCount;
		};

		constexpr std::array<DomainInfo, 2> Domains { {
			{ "sunwing", 0xE9AC4FB, 0xE9AC88B, 0x1003D030, 0x449, 0x1003F280, 0x100411C0, 0xF9C, 0x10048300, 0x135 },
			{ "waterwing", 0xB817D9C, 0xB81810C, 0xFF5C450, 0x49B, 0xFF5E930, 0xFF60B20, 0x10F7, 0xFF68600, 0x147 },
		} };

		constexpr uint8_t DispatchPrologue[] = { 0x56, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };
		constexpr uint8_t BehavePrologue[] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55 };

		using DispatchFn = uint64_t (*)(void *, void *, void *, void *, uint64_t, uint64_t);
		using BehaveFn = uint64_t (*)(void *, void *, void *, void *);

		struct DomainState
		{
			DispatchFn m_Dispatch = nullptr;
			BehaveFn m_Behave = nullptr;
			bool m_Hooked = false;
			std::atomic<uintptr_t> m_Pool = 0;
			std::unordered_map<uint64_t, std::string> m_SymbolNames; // Symbol value -> name
			std::unordered_map<std::string, uint64_t> m_SymbolValues;
			std::unordered_set<uintptr_t> m_Agents;
		};

		uintptr_t g_Base = 0;
		std::atomic_bool g_Installed = false;
		std::array<DomainState, Domains.size()> g_States;

		std::mutex g_Mutex;
		std::vector<Patch> g_Patches;

		std::atomic_uint64_t g_DumpUntil = 0;
		std::unordered_set<uintptr_t> g_Dumped; // Guarded by g_Mutex

		bool SafeCopy(void *Destination, const void *Source, size_t Size)
		{
			__try
			{
				memcpy(Destination, Source, Size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		template<typename T>
		const T *At(uintptr_t Rva)
		{
			return reinterpret_cast<const T *>(g_Base + Rva);
		}

		std::string PredicateName(size_t Domain, uint32_t Index)
		{
			const auto& info = Domains[Domain];
			return Index < info.m_PredicateCount ? At<const char *>(info.m_Predicates)[Index] : std::format("#{}", Index);
		}

		void BuildSymbolsLocked(size_t Domain, uintptr_t Pool)
		{
			const auto& info = Domains[Domain];
			auto& state = g_States[Domain];
			const auto names = At<const char *>(info.m_Names);
			const auto indices = At<uint16_t>(info.m_SymbolIndices);
			const auto deltas = At<uint8_t>(info.m_SymbolDeltas);

			state.m_SymbolNames.clear();
			state.m_SymbolValues.clear();
			size_t offset = 0;

			for (uint32_t i = 0; i < info.m_SymbolCells; i++)
			{
				offset += deltas[i] * 8;
				uint64_t value = 0;

				if (indices[i] >= info.m_NameCount || !SafeCopy(&value, reinterpret_cast<void *>(Pool + offset + 8), sizeof(value)))
					continue;

				state.m_SymbolNames.emplace(value, names[indices[i]]);
				state.m_SymbolValues.emplace(names[indices[i]], value);
			}
		}

		std::optional<uint64_t> ParseValueLocked(size_t Domain, const std::string& Text)
		{
			const auto& info = Domains[Domain];

			if (Text.starts_with("pred:"))
			{
				const auto name = Text.substr(5);

				for (uint32_t i = 0; i < info.m_PredicateCount; i++)
				{
					if (name == At<const char *>(info.m_Predicates)[i])
						return i;
				}

				return std::nullopt;
			}

			if (Text.starts_with("sym:"))
			{
				const auto& values = g_States[Domain].m_SymbolValues;

				if (auto itr = values.find(Text.substr(4)); itr != values.end())
					return itr->second;

				return std::nullopt;
			}

			int64_t value = 0;
			const bool hex = Text.starts_with("0x");
			const auto begin = Text.data() + (hex ? 2 : 0);

			if (std::from_chars(begin, Text.data() + Text.size(), value, hex ? 16 : 10).ec != std::errc())
				return std::nullopt;

			return static_cast<uint64_t>(value);
		}

		void ApplyPatchLocked(Patch& P, size_t Domain, uintptr_t Pool)
		{
			const auto address = reinterpret_cast<uint64_t *>(Pool + P.m_Offset);

			if (P.m_Enabled && !P.m_Applied)
			{
				const auto value = ParseValueLocked(Domain, P.m_Value);

				if (!value)
				{
					P.m_Error = "unknown value";
					return;
				}

				if (!SafeCopy(&P.m_Original, address, sizeof(uint64_t)) || !SafeCopy(address, &*value, sizeof(uint64_t)))
				{
					P.m_Error = "pool not writable";
					return;
				}

				P.m_Applied = true;
				P.m_Error.clear();
				spdlog::info("[htn] {} pool+{:#x}: {:#x} -> {:#x} ({})", P.m_Domain, P.m_Offset, P.m_Original, *value, P.m_Value);
			}
			else if (!P.m_Enabled && P.m_Applied)
			{
				SafeCopy(address, &P.m_Original, sizeof(uint64_t));
				P.m_Applied = false;
				spdlog::info("[htn] {} pool+{:#x}: restored {:#x}", P.m_Domain, P.m_Offset, P.m_Original);
			}
		}

		void ApplyPatchesLocked(size_t Domain)
		{
			const auto pool = g_States[Domain].m_Pool.load();

			if (!pool)
				return;

			for (auto& p : g_Patches)
			{
				if (p.m_Domain == Domains[Domain].m_Name)
					ApplyPatchLocked(p, Domain, pool);
			}
		}

		void OnPool(size_t Domain, uintptr_t Pool)
		{
			auto& state = g_States[Domain];

			if (state.m_Pool.load(std::memory_order_relaxed) == Pool)
				return;

			std::scoped_lock lock(g_Mutex);

			if (state.m_Pool.load() == Pool)
				return;

			// A new pool (domain loaded again) starts from the original cells
			for (auto& p : g_Patches)
			{
				if (p.m_Domain == Domains[Domain].m_Name)
					p.m_Applied = false;
			}

			state.m_Pool = Pool;
			BuildSymbolsLocked(Domain, Pool);
			spdlog::info("[htn] {} domain pool at {:#x} ({} symbols)", Domains[Domain].m_Name, Pool, state.m_SymbolNames.size());
			ApplyPatchesLocked(Domain);
		}

		std::string FormatQword(size_t Domain, uint64_t Value)
		{
			const auto& names = g_States[Domain].m_SymbolNames;

			if (auto itr = names.find(Value); itr != names.end())
				return itr->second;

			if (Value < 0x10000)
				return std::to_string(Value);

			double d;
			memcpy(&d, &Value, sizeof(d));

			if (std::abs(d) > 1e-3 && std::abs(d) < 1e7)
				return std::format("{:#x}({:.2f})", Value, d);

			return std::format("{:#x}", Value);
		}

		// World state: one linked list of facts per predicate; nodes are dumped raw with known symbols named
		void DumpWorldState(size_t Domain, uintptr_t WorldState)
		{
			const auto& info = Domains[Domain];
			std::string text = std::format("[htn] {} agent world state {:#x}:", info.m_Name, WorldState);

			for (uint32_t i = 0; i < info.m_PredicateCount; i++)
			{
				uintptr_t node = 0;

				if (!SafeCopy(&node, reinterpret_cast<void *>(WorldState + i * 8), sizeof(node)) || !node)
					continue;

				text += std::format("\n  {}:", PredicateName(Domain, i));

				for (int n = 0; node && n < 12; n++)
				{
					uint64_t q[7] = {};

					if (!SafeCopy(q, reinterpret_cast<void *>(node), sizeof(q)))
						break;

					text += "\n    (";

					for (int k = 1; k < 7; k++)
						text += (k > 1 ? " " : "") + FormatQword(Domain, q[k]);

					text += ")";
					node = static_cast<uintptr_t>(q[0]);
				}
			}

			spdlog::info("{}", text);
		}

		template<size_t D>
		uint64_t HookDispatch(void *Pool, void *A, void *B, void *C, uint64_t E, uint64_t F)
		{
			OnPool(D, reinterpret_cast<uintptr_t>(Pool));
			return g_States[D].m_Dispatch(Pool, A, B, C, E, F);
		}

		template<size_t D>
		uint64_t HookBehave(void *Planner, void *WorldState, void *Frame, void *R9)
		{
			const auto ws = reinterpret_cast<uintptr_t>(WorldState);
			bool dump = false;

			{
				std::scoped_lock lock(g_Mutex);
				g_States[D].m_Agents.insert(ws);

				if (GetTickCount64() < g_DumpUntil.load() && g_Dumped.insert(ws).second)
					dump = true;
			}

			if (dump)
				DumpWorldState(D, ws);

			return g_States[D].m_Behave(Planner, WorldState, Frame, R9);
		}

		template<size_t D>
		void InstallDomain()
		{
			const auto& info = Domains[D];
			auto& state = g_States[D];
			const auto dispatch = reinterpret_cast<void *>(g_Base + info.m_Dispatch);
			const auto behave = reinterpret_cast<void *>(g_Base + info.m_Behave);

			if (memcmp(dispatch, DispatchPrologue, sizeof(DispatchPrologue)) != 0 || memcmp(behave, BehavePrologue, sizeof(BehavePrologue)) != 0)
			{
				spdlog::warn("[htn] {} domain code does not match this game build, skipped", info.m_Name);
				return;
			}

			state.m_Dispatch = reinterpret_cast<DispatchFn>(dispatch);
			state.m_Behave = reinterpret_cast<BehaveFn>(behave);

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<void **>(&state.m_Dispatch), reinterpret_cast<void *>(&HookDispatch<D>));
			DetourAttach(reinterpret_cast<void **>(&state.m_Behave), reinterpret_cast<void *>(&HookBehave<D>));

			if (DetourTransactionCommit() != NO_ERROR)
			{
				spdlog::error("[htn] failed to hook the {} domain", info.m_Name);
				return;
			}

			state.m_Hooked = true;
			spdlog::info("[htn] hooked the {} domain", info.m_Name);
		}
	}

	void Initialize()
	{
		std::scoped_lock lock(g_Mutex);

		for (const auto& c : ModConfiguration.HtnPatches)
		{
			Patch p;
			p.m_Enabled = c.Enabled;
			p.m_Domain = c.Domain;
			p.m_Value = c.Value;

			uint32_t offset = 0;
			const bool hex = c.Offset.starts_with("0x");
			const auto begin = c.Offset.data() + (hex ? 2 : 0);

			if (std::from_chars(begin, c.Offset.data() + c.Offset.size(), offset, hex ? 16 : 10).ec != std::errc() || offset % 8 != 0)
			{
				spdlog::error("[htn] invalid [[HTNPatch]] offset '{}'", c.Offset);
				continue;
			}

			p.m_Offset = offset;
			g_Patches.emplace_back(std::move(p));
		}
	}

	void TryInstall()
	{
		if (g_Installed.load() || (!ModConfiguration.HtnDebug && g_Patches.empty()))
			return;

		const auto module = GetModuleHandleW(L"fullgame.dll");

		if (!module)
			return;

		g_Base = reinterpret_cast<uintptr_t>(module);
		g_Installed = true;

		InstallDomain<0>();
		InstallDomain<1>();
	}

	std::vector<DomainStatus> GetDomains()
	{
		std::scoped_lock lock(g_Mutex);
		std::vector<DomainStatus> result;

		for (size_t i = 0; i < Domains.size(); i++)
		{
			DomainStatus s;
			s.m_Name = Domains[i].m_Name;
			s.m_Hooked = g_States[i].m_Hooked;
			s.m_Pool = g_States[i].m_Pool;
			s.m_Agents = g_States[i].m_Agents.size();

			for (const auto& p : g_Patches)
			{
				if (p.m_Domain == s.m_Name && p.m_Applied)
					s.m_PatchesApplied++;
			}

			result.emplace_back(s);
		}

		return result;
	}

	std::vector<Patch> GetPatches()
	{
		std::scoped_lock lock(g_Mutex);
		return g_Patches;
	}

	void SetPatchEnabled(size_t Index, bool Enabled)
	{
		std::scoped_lock lock(g_Mutex);

		if (Index >= g_Patches.size())
			return;

		auto& p = g_Patches[Index];
		p.m_Enabled = Enabled;

		for (size_t d = 0; d < Domains.size(); d++)
		{
			if (p.m_Domain == Domains[d].m_Name && g_States[d].m_Pool)
				ApplyPatchLocked(p, d, g_States[d].m_Pool);
		}
	}

	void RequestWorldStateDump(uint32_t Milliseconds)
	{
		std::scoped_lock lock(g_Mutex);
		g_Dumped.clear();
		g_DumpUntil = GetTickCount64() + Milliseconds;
	}
}
