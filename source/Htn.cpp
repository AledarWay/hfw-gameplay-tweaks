#include <Windows.h>
#include <detours/detours.h>
#include <array>
#include <string_view>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <optional>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
			uintptr_t m_CodeDeltas; // uint8 delta (in qwords) to the next code pointer cell
			uint32_t m_CodeCells;
		};

		constexpr std::array<DomainInfo, 2> Domains { {
			{ "sunwing", 0xE9AC4FB, 0xE9AC88B, 0x1003D030, 0x449, 0x1003F280, 0x100411C0, 0xF9C, 0x10048300, 0x135, 0x1004F870, 0xE46 },
			{ "waterwing", 0xB817D9C, 0xB81810C, 0xFF5C450, 0x49B, 0xFF5E930, 0xFF60B20, 0x10F7, 0xFF68600, 0x147, 0xFF706E0, 0xFAF },
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
			std::unordered_map<uintptr_t, std::string> m_LastTrace; // Agent -> last logged plan summary
		};

		// A compiled task, method or condition reports success by calling its continuation. Returning without
		// calling it is a failure, so the planner moves on to the next alternative.
		void FailCell(void *, void *, void *)
		{
		}

		// Cells that depend on the agent: "mark-mount" remembers which world state reached the player's mount
		// branch, "fail-if-mount" fails only for that agent and "pass-if-mount" makes a condition succeed for it.
		// Code cells get (planner, world state, frame); a condition's frame holds its continuation at +0x10.
		using CellFn = uint64_t (*)(void *, void *, void *, void *);

		enum class CellKind
		{
			MarkMount,
			FailIfMount,
			PassIfMount,
			FailWhileTesting,
		};

		constexpr size_t MaxConditionalCells = 16;
		std::array<std::atomic_uint64_t, MaxConditionalCells> g_CellOriginals;
		size_t g_UsedConditionalCells = 0;
		thread_local void *g_MountWorldState = nullptr;

		// pass-if-mount forces the condition in windows only, so the mount still picks other attacks in between
		std::atomic_uint64_t g_MountActsAloneUntil = 0; // "fail-while-testing" cells fail until then
		std::atomic_uint64_t g_ForceShotUntil = 0;		 // pass-if-mount passes and the shot needs no position until then
		std::atomic_uint64_t g_PassWindowMs = 6000;
		std::atomic_uint64_t g_PassCooldownMs = 12000;
		std::array<std::atomic_uint64_t, MaxConditionalCells> g_PassWindowStart;

		template<CellKind K, size_t N>
		uint64_t ConditionalCell(void *Planner, void *WorldState, void *Frame, void *R9)
		{
			if constexpr (K == CellKind::MarkMount)
				g_MountWorldState = WorldState;

			if constexpr (K == CellKind::FailIfMount)
			{
				if (WorldState == g_MountWorldState)
					return 0;
			}

			if constexpr (K == CellKind::FailWhileTesting)
			{
				if (GetTickCount64() < g_MountActsAloneUntil.load())
					return 0;
			}

			if constexpr (K == CellKind::PassIfMount)
			{
				const auto now = GetTickCount64();
				auto start = g_PassWindowStart[N].load();

				const auto window = g_PassWindowMs.load();

				if (now - start >= window + g_PassCooldownMs.load())
				{
					g_PassWindowStart[N] = now;
					start = now;
				}

				if (WorldState == g_MountWorldState && (now - start < window || now < g_ForceShotUntil.load()))
				{
					const auto next = *reinterpret_cast<uint64_t **>(reinterpret_cast<uintptr_t>(Frame) + 0x10);
					return reinterpret_cast<CellFn>(*next)(Planner, WorldState, next, R9);
				}
			}

			return reinterpret_cast<CellFn>(g_CellOriginals[N].load())(Planner, WorldState, Frame, R9);
		}

		template<CellKind K, size_t... I>
		constexpr auto MakeCellTable(std::index_sequence<I...>)
		{
			return std::array<CellFn, sizeof...(I)> { &ConditionalCell<K, I>... };
		}

		const auto MarkMountCells = MakeCellTable<CellKind::MarkMount>(std::make_index_sequence<MaxConditionalCells>());
		const auto FailIfMountCells = MakeCellTable<CellKind::FailIfMount>(std::make_index_sequence<MaxConditionalCells>());
		const auto PassIfMountCells = MakeCellTable<CellKind::PassIfMount>(std::make_index_sequence<MaxConditionalCells>());
		const auto FailWhileTestingCells = MakeCellTable<CellKind::FailWhileTesting>(std::make_index_sequence<MaxConditionalCells>());

		uintptr_t g_Base = 0;
		std::atomic_bool g_Installed = false;
		std::array<DomainState, Domains.size()> g_States;

		std::mutex g_Mutex;
		std::vector<Patch> g_Patches;

		std::atomic_uint64_t g_DumpUntil = 0;

		// Decision trace: every code pointer cell in a pool is redirected through a stub that records which
		// cells (subtasks, methods, primitives) each agent runs, in order of first use
		struct TraceAgent
		{
			std::vector<uint32_t> m_Order;
			std::unordered_set<uint32_t> m_Seen;
		};

		struct TraceDomain
		{
			uint8_t *m_Stubs = nullptr;
			std::vector<uint32_t> m_Offsets;   // Pool offset per traced cell
			std::vector<uint64_t> m_Originals; // Original code pointer per traced cell
			bool m_Active = false;
		};

		constexpr size_t StubSize = 64;
		std::array<TraceDomain, 2> g_Trace;
		std::atomic_bool g_Tracing = false;
		std::atomic_uint64_t g_TraceUntil = 0;
		std::mutex g_TraceMutex;
		std::array<std::unordered_map<uintptr_t, TraceAgent>, 2> g_TraceAgents;
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

			if (Text == "fail")
				return reinterpret_cast<uint64_t>(&FailCell);

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
				const bool conditional = P.m_Value == "mark-mount" || P.m_Value == "fail-if-mount" || P.m_Value == "pass-if-mount" ||
										 P.m_Value == "fail-while-testing";
				std::optional<uint64_t> value;

				if (!SafeCopy(&P.m_Original, address, sizeof(uint64_t)))
				{
					P.m_Error = "pool not readable";
					return;
				}

				if (conditional)
				{
					if (P.m_Slot < 0)
					{
						if (g_UsedConditionalCells >= MaxConditionalCells)
						{
							P.m_Error = "too many conditional cells";
							return;
						}

						P.m_Slot = static_cast<int>(g_UsedConditionalCells++);
					}

					g_CellOriginals[P.m_Slot] = P.m_Original;
					const auto& table = P.m_Value == "mark-mount"		? MarkMountCells
									 : P.m_Value == "fail-if-mount" ? FailIfMountCells
									 : P.m_Value == "pass-if-mount" ? PassIfMountCells
																	: FailWhileTestingCells;
					value = reinterpret_cast<uint64_t>(table[P.m_Slot]);
				}
				else
				{
					value = ParseValueLocked(Domain, P.m_Value);
				}

				if (!value)
				{
					P.m_Error = "unknown value";
					return;
				}

				if (!SafeCopy(address, &*value, sizeof(uint64_t)))
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

		void TraceHit(uint32_t Id, uintptr_t WorldState)
		{
			const size_t domain = Id >> 24;
			const uint32_t cell = Id & 0xFFFFFF;

			if (domain >= g_TraceAgents.size())
				return;

			std::scoped_lock lock(g_TraceMutex);
			auto& agent = g_TraceAgents[domain][WorldState];

			if (agent.m_Seen.insert(cell).second)
				agent.m_Order.emplace_back(cell);
		}

		void EmitStub(uint8_t *Out, uint32_t Id, uint64_t Original)
		{
			const uint64_t logger = reinterpret_cast<uint64_t>(&TraceHit);
			uint8_t code[] = {
				0x51,							  // push rcx
				0x52,							  // push rdx
				0x41, 0x50,						  // push r8
				0x41, 0x51,						  // push r9
				0x48, 0x83, 0xEC, 0x28,			  // sub rsp, 0x28
				0xB9, 0, 0, 0, 0,				  // mov ecx, Id (rdx = world state)
				0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, TraceHit
				0xFF, 0xD0,						  // call rax
				0x48, 0x83, 0xC4, 0x28,			  // add rsp, 0x28
				0x41, 0x59,						  // pop r9
				0x41, 0x58,						  // pop r8
				0x5A,							  // pop rdx
				0x59,							  // pop rcx
				0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, Original
				0xFF, 0xE0,						  // jmp rax
			};

			memcpy(&code[11], &Id, 4);
			memcpy(&code[17], &logger, 8);
			memcpy(&code[39], &Original, 8);
			memset(Out, 0xCC, StubSize);
			memcpy(Out, code, sizeof(code));
		}

		bool InsideModule(uint64_t Address)
		{
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_Base + reinterpret_cast<const IMAGE_DOS_HEADER *>(g_Base)->e_lfanew);
			return Address >= g_Base && Address < g_Base + nt->OptionalHeader.SizeOfImage;
		}

		void StartTraceDomain(size_t Domain)
		{
			const auto& info = Domains[Domain];
			auto& trace = g_Trace[Domain];
			const auto pool = g_States[Domain].m_Pool.load();

			if (!pool || trace.m_Active)
				return;

			if (trace.m_Offsets.empty())
			{
				const auto deltas = At<uint8_t>(info.m_CodeDeltas);
				size_t offset = 0;

				for (uint32_t i = 0; i < info.m_CodeCells; i++)
				{
					offset += deltas[i] * 8;
					trace.m_Offsets.emplace_back(static_cast<uint32_t>(offset));
				}

				trace.m_Stubs = static_cast<uint8_t *>(VirtualAlloc(nullptr, trace.m_Offsets.size() * StubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

				if (!trace.m_Stubs)
				{
					spdlog::error("[htn] trace: no memory for stubs");
					trace.m_Offsets.clear();
					return;
				}
			}

			trace.m_Originals.assign(trace.m_Offsets.size(), 0);
			size_t redirected = 0;

			for (size_t i = 0; i < trace.m_Offsets.size(); i++)
			{
				auto cell = reinterpret_cast<uint64_t *>(pool + trace.m_Offsets[i]);
				uint64_t original = 0;

				if (!SafeCopy(&original, cell, sizeof(original)) || !InsideModule(original))
					continue;

				trace.m_Originals[i] = original;
				const auto stub = trace.m_Stubs + i * StubSize;
				EmitStub(stub, static_cast<uint32_t>((Domain << 24) | i), original);

				const uint64_t value = reinterpret_cast<uint64_t>(stub);
				SafeCopy(cell, &value, sizeof(value));
				redirected++;
			}

			FlushInstructionCache(GetCurrentProcess(), trace.m_Stubs, trace.m_Offsets.size() * StubSize);
			trace.m_Active = true;
			spdlog::info("[htn] trace: {} code cells of the {} domain redirected", redirected, info.m_Name);
		}

		void StopTraceDomain(size_t Domain)
		{
			auto& trace = g_Trace[Domain];
			const auto pool = g_States[Domain].m_Pool.load();

			if (!trace.m_Active)
				return;

			// Stubs stay allocated: frames copied during the trace may still call them
			for (size_t i = 0; i < trace.m_Offsets.size(); i++)
			{
				if (trace.m_Originals[i] && pool)
					SafeCopy(reinterpret_cast<void *>(pool + trace.m_Offsets[i]), &trace.m_Originals[i], sizeof(uint64_t));
			}

			trace.m_Active = false;

			std::scoped_lock lock(g_TraceMutex);

			for (auto& [ws, agent] : g_TraceAgents[Domain])
			{
				std::string text = std::format("[htn] trace {} agent {:#x}: {} cells, pool offsets in order of first use:", Domains[Domain].m_Name, ws, agent.m_Order.size());

				for (size_t i = 0; i < agent.m_Order.size(); i++)
					text += std::format("{}{:x}", i % 24 == 0 ? "\n  " : " ", trace.m_Offsets[agent.m_Order[i]]);

				spdlog::info("{}", text);
			}

			g_TraceAgents[Domain].clear();
		}

		void UpdateTrace()
		{
			if (!g_Tracing.load(std::memory_order_relaxed))
				return;

			std::scoped_lock lock(g_Mutex);

			if (!g_Tracing.load())
				return;

			if (GetTickCount64() < g_TraceUntil.load())
			{
				for (size_t d = 0; d < Domains.size(); d++)
					StartTraceDomain(d);

				return;
			}

			for (size_t d = 0; d < Domains.size(); d++)
				StopTraceDomain(d);

			g_Tracing = false;
			spdlog::info("[htn] trace finished");
		}

		template<size_t D>
		uint64_t HookDispatch(void *Pool, void *A, void *B, void *C, uint64_t E, uint64_t F)
		{
			OnPool(D, reinterpret_cast<uintptr_t>(Pool));
			return g_States[D].m_Dispatch(Pool, A, B, C, E, F);
		}

		// First argument of the first fact of a predicate, as a name when it is a known symbol
		std::string FirstArgument(size_t Domain, uintptr_t WorldState, const char *Predicate)
		{
			const auto& info = Domains[Domain];

			for (uint32_t i = 0; i < info.m_PredicateCount; i++)
			{
				if (strcmp(At<const char *>(info.m_Predicates)[i], Predicate) != 0)
					continue;

				uintptr_t node = 0;
				uint64_t q[4] = {};

				if (!SafeCopy(&node, reinterpret_cast<void *>(WorldState + i * 8), sizeof(node)) || !node ||
					!SafeCopy(q, reinterpret_cast<void *>(node), sizeof(q)))
					return "-";

				std::scoped_lock lock(g_Mutex);
				return FormatQword(Domain, q[3]);
			}

			return "?";
		}

		// Logs when an agent's active plan, behavior, mover or sensor set changes
		void TracePlan(size_t Domain, uintptr_t WorldState)
		{
			const auto summary = std::format(
				"plan={} behavior={} mover={} sensors={}",
				FirstArgument(Domain, WorldState, "active_plan"),
				FirstArgument(Domain, WorldState, "current_behavior"),
				FirstArgument(Domain, WorldState, "mover"),
				FirstArgument(Domain, WorldState, "sensor_set"));

			{
				std::scoped_lock lock(g_Mutex);
				auto& last = g_States[Domain].m_LastTrace[WorldState];

				if (last == summary)
					return;

				last = summary;
			}

			spdlog::info("[htn] {} agent {:#x}: {}", Domains[Domain].m_Name, WorldState, summary);
		}

		// Sunwing shoot_from_distance only fires once (at_shoot_from_distance_position) holds, which needs the mount to
		// fly there first. While forcing a shot the check reads "player" instead, a predicate every agent has.
		constexpr uint32_t ShotPositionCell = 0x553C0;
		constexpr uint64_t ShotPositionPredicate = 301; // at_shoot_from_distance_position
		constexpr uint64_t AlwaysTruePredicate = 12;	// player
		bool g_ShotPatched = false;

		void UpdateForcedShotLocked()
		{
			const auto pool = g_States[0].m_Pool.load();

			if (!pool)
				return;

			const auto cell = reinterpret_cast<uint64_t *>(pool + ShotPositionCell);
			const bool want = GetTickCount64() < g_ForceShotUntil.load();

			if (want && !g_ShotPatched)
			{
				uint64_t value = 0;

				if (!SafeCopy(&value, cell, sizeof(value)) || value != ShotPositionPredicate)
					return;

				SafeCopy(cell, &AlwaysTruePredicate, sizeof(uint64_t));
				g_ShotPatched = true;
				spdlog::info("[htn] forced shot: position check skipped");
			}
			else if (!want && g_ShotPatched)
			{
				SafeCopy(cell, &ShotPositionPredicate, sizeof(uint64_t));
				g_ShotPatched = false;
				spdlog::info("[htn] forced shot: position check restored");
			}
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

				if constexpr (D == 0)
					UpdateForcedShotLocked();
			}

			if (dump)
				DumpWorldState(D, ws);

			UpdateTrace();

			const auto result = g_States[D].m_Behave(Planner, WorldState, Frame, R9);

			if (ModConfiguration.HtnDebug)
				TracePlan(D, ws);

			return result;
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

		if (ModConfiguration.CalledFlyingMountFights)
		{
			// Flying mounts pick "player's mount" before combat: when that method finds anything to do, the mount never
			// reaches circle_combat. Failing its idle alternatives lets the planner fall through to combat, the same
			// path a second overridden mount takes. Calling, pick-up and riding are separate alternatives.
			const auto& c = ModConfiguration.FlyingMountAI;

			g_PassWindowMs = static_cast<uint64_t>(std::max(c.RangedAttackWindow, 0.0) * 1000.0);
			g_PassCooldownMs = static_cast<uint64_t>(std::max(c.RangedAttackCooldown, 0.0) * 1000.0);

			struct Preset
			{
				const char *m_Domain;
				uint32_t m_Offset;
				const char *m_Value;
				bool m_Enabled;
				const char *m_Label;
			};

			const Preset presets[] = {
				{ "sunwing", 0xBFD0, "mark-mount", true, "Sunwing: detect the called mount" },
				{ "sunwing", 0x24C18, "fail", c.SkipIdleInAir, "Sunwing: in the air, skip go_to_hacker/glide_patrol" },
				{ "sunwing", 0x24C50, "fail", c.SkipIdleInAir, "Sunwing: in the air, skip wait_hacked (continue)" },
				{ "sunwing", 0x24C70, "fail", c.SkipIdleInAir, "Sunwing: in the air, skip wait_hacked (begin)" },
				{ "sunwing", 0x24FE0, "fail", c.SkipIdleLanded, "Sunwing: landed, skip wait_hacked (continue)" },
				{ "sunwing", 0x25000, "fail", c.SkipIdleLanded, "Sunwing: landed, skip wait_hacked (begin)" },
				{ "sunwing", 0x24F10, "fail", c.SkipGoToHackerLanded, "Sunwing: landed, skip go_to_hacker" },
				{ "sunwing", 0x194F0, "fail", c.SkipWholeLandedBranch, "Sunwing: landed, skip the whole mount branch" },
				{ "sunwing", 0x49E48, "fail-if-mount", c.NoLandingNextToTarget, "Sunwing mount: don't land next to the target" },
				{ "sunwing", 0x497A0, "pass-if-mount", c.FlyingRangedAttack, "Sunwing mount: allow the flying plasma barrage" },

				{ "waterwing", 0xAFE0, "mark-mount", true, "Waterwing: detect the called mount" },
				{ "waterwing", 0x21A48, "fail", c.SkipIdleInAir, "Waterwing: in the air, skip wait_hacked (continue)" },
				{ "waterwing", 0x21A68, "fail", c.SkipIdleInAir, "Waterwing: in the air, skip wait_hacked (begin)" },
				{ "waterwing", 0x22200, "fail", c.SkipIdleLanded, "Waterwing: landed, skip wait_hacked (continue)" },
				{ "waterwing", 0x22220, "fail", c.SkipIdleLanded, "Waterwing: landed, skip wait_hacked (begin)" },
				{ "waterwing", 0x22130, "fail", c.SkipGoToHackerLanded, "Waterwing: landed, skip go_to_hacker" },
				{ "waterwing", 0x21E70, "fail", c.SkipIdleInWater, "Waterwing: in water, skip wait_hacked (continue)" },
				{ "waterwing", 0x21E90, "fail", c.SkipIdleInWater, "Waterwing: in water, skip wait_hacked (begin)" },
				{ "waterwing", 0x21E08, "fail", c.SkipSwimNearHacker, "Waterwing: in water, skip swim_near_hacker_and_wait" },
				{ "waterwing", 0x30F98, "fail-if-mount", c.NoLandingNextToTarget, "Waterwing mount: don't land next to the target" },
				{ "waterwing", 0x513F8, "pass-if-mount", c.FlyingRangedAttack, "Waterwing mount: allow the flying gravel spit" },
			};

			for (const auto& preset : presets)
			{
				const bool sunwing = std::string_view(preset.m_Domain) == "sunwing";

				if (!(sunwing ? c.Sunwing : c.Waterwing))
					continue;

				Patch p;
				p.m_Enabled = preset.m_Enabled;
				p.m_Domain = preset.m_Domain;
				p.m_Offset = preset.m_Offset;
				p.m_Value = preset.m_Value;
				p.m_Label = preset.m_Label;
				g_Patches.emplace_back(std::move(p));
			}
		}

		// While ridden the mount only runs do_mounted (wait). Failing it for a moment lets the planner pick what an
		// unridden mount would do, e.g. attack, to see how the game handles that with a rider.
		constexpr std::pair<const char *, uint32_t> mountedCells[] = {
			{ "sunwing", 0x2980 },
			{ "sunwing", 0x29A0 },
			{ "waterwing", 0x26D0 },
			{ "waterwing", 0x26F0 },
		};

		for (const auto& [domain, offset] : mountedCells)
		{
			Patch p;
			p.m_Domain = domain;
			p.m_Offset = offset;
			p.m_Value = "fail-while-testing";
			p.m_Label = std::format("{}: test hook for do_mounted (+{:#x})", domain, offset);
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

	void RequestMountActsAlone(uint32_t Milliseconds)
	{
		g_MountActsAloneUntil = GetTickCount64() + Milliseconds;
		spdlog::info("[htn] ridden mount acts on its own for {} ms", Milliseconds);
	}

	void RequestForcedShot(uint32_t Milliseconds)
	{
		const auto until = GetTickCount64() + Milliseconds;
		g_ForceShotUntil = until;
		g_MountActsAloneUntil = until;
		spdlog::info("[htn] forcing a Sunwing plasma barrage for {} ms", Milliseconds);
	}

	uint32_t GetMountActsAloneRemaining()
	{
		const auto now = GetTickCount64();
		const auto until = g_MountActsAloneUntil.load();
		return until > now ? static_cast<uint32_t>(until - now) : 0;
	}

	void RequestDecisionTrace(uint32_t Milliseconds)
	{
		{
			std::scoped_lock lock(g_TraceMutex);

			for (auto& agents : g_TraceAgents)
				agents.clear();
		}

		g_TraceUntil = GetTickCount64() + Milliseconds;
		g_Tracing = true;
	}

	void RequestWorldStateDump(uint32_t Milliseconds)
	{
		std::scoped_lock lock(g_Mutex);
		g_Dumped.clear();
		g_DumpUntil = GetTickCount64() + Milliseconds;
	}
}
