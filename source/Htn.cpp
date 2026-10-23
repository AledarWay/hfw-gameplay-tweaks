#include <Windows.h>
#include <detours/detours.h>
#include <array>
#include <bit>
#include <string_view>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <optional>
#include <cstring>
#include <format>
#include <mutex>
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
			uintptr_t m_Dispatch; // (pool, ?, ?, task cell, arity) -> compiled task
		};

		constexpr std::array<DomainInfo, 2> Domains { {
			{ "sunwing", 0xE9AC4FB },
			{ "waterwing", 0xB817D9C },
		} };

		constexpr uint8_t DispatchPrologue[] = { 0x56, 0x48, 0x81, 0xEC, 0xF0, 0x00, 0x00, 0x00 };

		using DispatchFn = uint64_t (*)(void *, void *, void *, void *, uint64_t, uint64_t);

		struct DomainState
		{
			DispatchFn m_Dispatch = nullptr;
			std::atomic<uintptr_t> m_Pool = 0;
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
			MarkMountInWater,
			FailIfMountInWater,
			DelayIfMount,
		};

		constexpr size_t MaxConditionalCells = 16;
		std::array<std::atomic_uint64_t, MaxConditionalCells> g_CellOriginals;
		size_t g_UsedConditionalCells = 0;
		thread_local void *g_MountWorldState = nullptr;
		thread_local bool g_MountInWater = false; // Set by "mark-mount-in-water" after "mark-mount" in the same pass

		// pass-if-mount forces the condition in windows only, so the mount still picks other attacks in between
		std::atomic_uint64_t g_PassWindowMs = 6000;
		std::atomic_uint64_t g_PassCooldownMs = 12000;
		std::array<std::atomic_uint64_t, MaxConditionalCells> g_PassWindowStart;

		// delay-if-mount fails until the cell has been reached continuously for this long
		std::atomic_uint64_t g_DelayMs = 3000;
		constexpr uint64_t DelayStreakGapMs = 1000;
		std::array<std::atomic_uint64_t, MaxConditionalCells> g_DelayFirst;
		std::array<std::atomic_uint64_t, MaxConditionalCells> g_DelayLast;

		template<CellKind K, size_t N>
		uint64_t ConditionalCell(void *Planner, void *WorldState, void *Frame, void *R9)
		{
			if constexpr (K == CellKind::MarkMount)
			{
				g_MountWorldState = WorldState;
				g_MountInWater = false;
			}

			if constexpr (K == CellKind::FailIfMountInWater)
			{
				if (WorldState == g_MountWorldState && g_MountInWater)
					return 0;
			}

			if constexpr (K == CellKind::DelayIfMount)
			{
				if (WorldState == g_MountWorldState)
				{
					const auto now = GetTickCount64();

					if (now - g_DelayLast[N].load() > DelayStreakGapMs)
						g_DelayFirst[N] = now;

					g_DelayLast[N] = now;

					if (now - g_DelayFirst[N].load() < g_DelayMs.load())
						return 0;
				}
			}

			if constexpr (K == CellKind::MarkMountInWater)
			{
				if (WorldState == g_MountWorldState)
					g_MountInWater = true;
			}

			if constexpr (K == CellKind::FailIfMount)
			{
				if (WorldState == g_MountWorldState)
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

				// The flying ranged attack can't run while swimming, forcing it there only stalls the mount
				if (WorldState == g_MountWorldState && !g_MountInWater && now - start < window)
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
		const auto MarkMountInWaterCells = MakeCellTable<CellKind::MarkMountInWater>(std::make_index_sequence<MaxConditionalCells>());
		const auto FailIfMountInWaterCells = MakeCellTable<CellKind::FailIfMountInWater>(std::make_index_sequence<MaxConditionalCells>());
		const auto DelayIfMountCells = MakeCellTable<CellKind::DelayIfMount>(std::make_index_sequence<MaxConditionalCells>());

		uintptr_t g_Base = 0;
		std::atomic_bool g_Installed = false;
		std::array<DomainState, Domains.size()> g_States;

		std::mutex g_Mutex;
		std::vector<Patch> g_Patches;

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

		std::optional<uint64_t> ParseValue(const std::string& Text)
		{
			if (Text.starts_with("double:"))
			{
				double value = 0.0;

				if (std::from_chars(Text.data() + 7, Text.data() + Text.size(), value).ec != std::errc())
					return std::nullopt;

				return std::bit_cast<uint64_t>(value);
			}

			if (Text == "fail")
				return reinterpret_cast<uint64_t>(&FailCell);

			return std::nullopt;
		}

		void ApplyPatchLocked(Patch& P, uintptr_t Pool)
		{
			const auto address = reinterpret_cast<uint64_t *>(Pool + P.m_Offset);

			if (P.m_Enabled && !P.m_Applied)
			{
				const bool conditional = P.m_Value == "mark-mount" || P.m_Value == "fail-if-mount" || P.m_Value == "pass-if-mount" ||
										 P.m_Value == "mark-mount-in-water" || P.m_Value == "fail-if-mount-in-water" ||
										 P.m_Value == "delay-if-mount";
				std::optional<uint64_t> value;

				if (!SafeCopy(&P.m_Original, address, sizeof(uint64_t)))
				{
					P.m_Error = "pool not readable";
					return;
				}

				if (P.m_Expected && P.m_Original != P.m_Expected)
				{
					P.m_Error = std::format("unexpected original {:#x}", P.m_Original);
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
									 : P.m_Value == "mark-mount-in-water" ? MarkMountInWaterCells
									 : P.m_Value == "fail-if-mount-in-water" ? FailIfMountInWaterCells
																			 : DelayIfMountCells;
					value = reinterpret_cast<uint64_t>(table[P.m_Slot]);
				}
				else
				{
					value = ParseValue(P.m_Value);
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
			}
			else if (!P.m_Enabled && P.m_Applied)
			{
				SafeCopy(address, &P.m_Original, sizeof(uint64_t));
				P.m_Applied = false;
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
					ApplyPatchLocked(p, pool);
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
			ApplyPatchesLocked(Domain);
		}

		template<size_t D>
		uint64_t HookDispatch(void *Pool, void *A, void *B, void *C, uint64_t E, uint64_t F)
		{
			OnPool(D, reinterpret_cast<uintptr_t>(Pool));
			return g_States[D].m_Dispatch(Pool, A, B, C, E, F);
		}

		template<size_t D>
		void InstallDomain()
		{
			const auto& info = Domains[D];
			auto& state = g_States[D];
			const auto dispatch = reinterpret_cast<void *>(g_Base + info.m_Dispatch);

			if (memcmp(dispatch, DispatchPrologue, sizeof(DispatchPrologue)) != 0)
			{
				spdlog::warn("[htn] {} domain code does not match this game build, skipped", info.m_Name);
				return;
			}

			state.m_Dispatch = reinterpret_cast<DispatchFn>(dispatch);

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<void **>(&state.m_Dispatch), reinterpret_cast<void *>(&HookDispatch<D>));

			if (DetourTransactionCommit() != NO_ERROR)
				spdlog::error("[htn] failed to hook the {} domain", info.m_Name);
		}
	}

	void Initialize()
	{
		if (!ModConfiguration.CalledFlyingMountFights)
			return;

		std::scoped_lock lock(g_Mutex);

		// Flying mounts pick "player's mount" before combat: when that method finds anything to do, the mount never
		// reaches circle_combat. Failing its idle alternatives lets the planner fall through to combat, the same
		// path a second overridden mount takes. Calling, pick-up and riding are separate alternatives.
		const auto& c = ModConfiguration.FlyingMountAI;

		g_PassWindowMs = static_cast<uint64_t>(std::max(c.RangedAttackWindow, 0.0) * 1000.0);
		g_PassCooldownMs = static_cast<uint64_t>(std::max(c.RangedAttackCooldown, 0.0) * 1000.0);
		g_DelayMs = static_cast<uint64_t>(std::max(c.OutOfWaterDelay, 0.0) * 1000.0);
		const auto lookAtThreat = std::format("double:{}", c.LookAtThreatSeconds);

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
			{ "sunwing", 0x4A0B0, lookAtThreat.c_str(), c.LookAtThreatSeconds > 0.0, "Sunwing: shorter look_at_threat (all Sunwings)" },

			{ "waterwing", 0xAFE0, "mark-mount", true, "Waterwing: detect the called mount" },
			{ "waterwing", 0x14CA0, "mark-mount-in-water", true, "Waterwing: detect the called mount in water" },
			{ "waterwing", 0x2E5A0, "fail", c.SkipIdleInAir, "Waterwing: in the air, skip go_to_hacker/glide_patrol" },
			{ "waterwing", 0x21A48, "fail", c.SkipIdleInAir, "Waterwing: in the air, skip wait_hacked (continue)" },
			{ "waterwing", 0x21A68, "fail", c.SkipIdleInAir, "Waterwing: in the air, skip wait_hacked (begin)" },
			{ "waterwing", 0x22200, "fail", c.SkipIdleLanded, "Waterwing: landed, skip wait_hacked (continue)" },
			{ "waterwing", 0x22220, "fail", c.SkipIdleLanded, "Waterwing: landed, skip wait_hacked (begin)" },
			{ "waterwing", 0x22130, "fail", c.SkipGoToHackerLanded, "Waterwing: landed, skip go_to_hacker" },
			{ "waterwing", 0x2ECD8, "fail", c.SkipIdleInWater, "Waterwing: in water, skip go_to_hacker/glide_patrol" },
			{ "waterwing", 0x21E70, "fail", c.SkipIdleInWater, "Waterwing: in water, skip wait_hacked (continue)" },
			{ "waterwing", 0x21E90, "fail", c.SkipIdleInWater, "Waterwing: in water, skip wait_hacked (begin)" },
			{ "waterwing", 0x21E08, "fail", c.SkipSwimNearHacker, "Waterwing: in water, skip swim_near_hacker_and_wait" },
			{ "waterwing", 0x30F98, "fail-if-mount", c.NoLandingNextToTarget, "Waterwing mount: don't land next to the target" },
			{ "waterwing", 0x59978, "fail-if-mount", c.NoLandingNextToTarget, "Waterwing mount: don't land next to the target (2)" },
			{ "waterwing", 0x513F8, "pass-if-mount", c.FlyingRangedAttack, "Waterwing mount: allow the flying gravel spit" },
			{ "waterwing", 0x5E770, "fail-if-mount-in-water", c.NoEvadeInWater, "Waterwing mount: no air dodge in water (continue)" },
			{ "waterwing", 0x5E880, "fail-if-mount-in-water", c.NoEvadeInWater, "Waterwing mount: no air dodge in water (begin)" },
			{ "waterwing", 0x587D0, "fail-if-mount-in-water", c.NoEvadeInWater, "Waterwing mount: no air dodge in water (continue 2)" },
			{ "waterwing", 0x58950, "fail-if-mount-in-water", c.NoEvadeInWater, "Waterwing mount: no air dodge in water (begin 2)" },
			{ "waterwing", 0x21458, "delay-if-mount", c.OutOfWaterDelay > 0.0, "Waterwing mount: wait before following the player out of water" },
			{ "waterwing", 0x59BE0, lookAtThreat.c_str(), c.LookAtThreatSeconds > 0.0, "Waterwing: shorter look_at_threat (all Waterwings)" },
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

			if (p.m_Value.starts_with("double:"))
				p.m_Expected = std::bit_cast<uint64_t>(10.0); // look_at_threat run_for_min in both domains

			g_Patches.emplace_back(std::move(p));
		}
	}

	void TryInstall()
	{
		if (g_Installed.load() || g_Patches.empty())
			return;

		const auto module = GetModuleHandleW(L"fullgame.dll");

		if (!module)
			return;

		g_Base = reinterpret_cast<uintptr_t>(module);
		g_Installed = true;

		InstallDomain<0>();
		InstallDomain<1>();
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
				ApplyPatchLocked(p, g_States[d].m_Pool);
		}
	}
}
