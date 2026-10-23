#include <Windows.h>
#include <detours/detours.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <random>
#include <utility>
#include "HRZ2/Core/Entity.h"
#include "HRZ2/Core/Player.h"
#include "HRZ2/Core/RTTI.h"
#include "HRZ2/Core/RTTIRefObject.h"
#include "HRZ2/Core/WorldTransform.h"
#include "HRZ2/GraphRuntime.h"
#include "ModConfiguration.h"
#include "Supercells.h"

namespace Supercells
{
	using namespace HRZ2;
	using namespace HRZ2::GraphRuntime;

	namespace
	{
		constexpr const char *ContainerTag = "SuperCellScriptContainer_eaae50a7816c4b1590d67387535ffbc9";
		constexpr const char *LimiterTag = "StateMachineNodeResource_04_517c22f7efbe578d0c63b931196e3d26";
		constexpr const char *WeatherTag = "StateMachineNodeResource_cfd23996b12f2166bf443d75dfb75cf5";
		constexpr const char *SceneUpdateEntry =
			"EntryPoint_SuperCellScriptContainer_eaae50a7816c4b1590d67387535ffbc9_5_HandleEvent_MsgSceneUpdate";

		constexpr auto PositionFact = GGUUID::Parse("9F73BE2D-14F3-A840-BD6A-172FF8623CEA");
		constexpr auto QuestInProgress = GGUUID::Parse("AE6A579F-D1DD-8441-856F-5599DB49825D");

		// Container state machine (MsgSceneUpdate, r8): state index at +0x28, per-state timers before it.
		// Cycle: 1 choose -> 6 forming -> 2 rise -> 7 active -> 4 end -> 5 dissipate -> 1; rejected picks wait in 3.
		constexpr int StateChoose = 1;
		constexpr int StateRetryWait = 3;
		constexpr int StateDissipate = 5;
		constexpr int StateForming = 6;
		constexpr int StateActive = 7;
		constexpr int StateDisabled = 11; // Entered when AETHER merges
		constexpr size_t StateOffset = 0x28;
		constexpr size_t FormingTimer = 0x14;
		constexpr size_t ActiveTimer = 0x1C;

		// Container graph instance (MsgSceneUpdate, r9), build HRZ2-PCR 80/9966125
		namespace Instance
		{
			constexpr size_t ChosenIndex = 0xB0;
			constexpr size_t Locations = 0xF0; // Array<AIMarker *>
			constexpr size_t Variants = 0x100; // Array<int>
			constexpr size_t Rotations = 0x110; // Array<float>
			constexpr size_t BlendRadii = 0x120; // Array<float>
			constexpr size_t Sizes = 0x130; // Array<16 bytes: inner, outer>
			constexpr size_t Flips = 0x140; // Array<bool>
			constexpr size_t Context = 0x150;
			constexpr size_t BlockFact0 = 0x1A8; // Blocks location 0 while in progress
			constexpr size_t InProgressEntry = 0x210;
			constexpr size_t PositionLink = 0x2A0;
			constexpr size_t RotationLink = 0x2A8;
			constexpr size_t VariantLink = 0x2B0;
			constexpr size_t BlendRadiusLink = 0x2B8;
			constexpr size_t InnerLink = 0x2C0;
			constexpr size_t OuterLink = 0x2C8;
			constexpr size_t FlipLink = 0x2D0;
			constexpr size_t LastIndex = 0x2D8;
			constexpr size_t BlockFact2 = 0x2E8; // Blocks location 2
			constexpr size_t BlockFact3 = 0x2F0; // Blocks location 3
		}

		// Native "world position of a marker" slot the container uses
		constexpr uintptr_t SlotMarkerPosition = 0x1052B108;

		constexpr int MaxLocations = 16;

		struct RawArray
		{
			int32_t m_Count;
			int32_t m_Capacity;
			uint8_t *m_Data;
		};

		struct LocationData
		{
			void *m_Marker = nullptr;
			bool m_HavePosition = false;
			double m_Position[3] = {};
			float m_Inner = 0.0f;
			float m_Outer = 0.0f;
			float m_BlendRadius = 0.0f;
			float m_Rotation = 0.0f;
			int m_Variant = 0;
			bool m_Flip = false;
		};

		using HandlerFn = uint64_t (*)(void *, void *, void *, void *, void *, void *);
		using MarkerPositionFn = void *(*)(double *, void *);

		HandlerFn g_OriginalSceneUpdate = nullptr;
		std::atomic_bool g_Installed = false;

		std::atomic_int g_State = -1;
		std::atomic<float> g_TimerCurrent = 0.0f;
		std::atomic<float> g_TimerEnd = 0.0f;
		std::atomic_uint64_t g_LastUpdateTick = 0;
		std::atomic_int g_GameIndex = -1;
		std::atomic_int g_CustomIndex = -1;
		std::atomic<uint8_t *> g_Instance = nullptr;

		std::atomic_bool g_Hold = false;
		float g_HeldTime = -1.0f;

		std::mutex g_PositionMutex;
		PositionMode g_PositionMode = PositionMode::Off;
		bool g_KeepAltitude = true;
		double g_LastPlayerOverride[3] = {};

		// Location selection, shared with the UI
		std::mutex g_SelectMutex;
		SelectionSettings g_Settings;
		std::vector<bool> g_GameEnabled(MaxLocations, true);
		int g_LocationCount = 0;
		bool g_LastEnabled = false; // The game's roll never reaches its last location
		std::vector<CustomLocation> g_Custom;
		int g_LastPick = -1; // >= 0 game, <= -2 custom (-2 - index)
		bool g_BlockOverridesDirty = true;

		std::mutex g_SnapshotMutex;
		std::vector<GameLocation> g_Snapshot;
		uint64_t g_SnapshotTick = 0;

		// Placed custom location that must survive the container re-applying its own
		std::optional<CustomLocation> g_ActiveCustom;
		double g_ActivePosition[3] = {};

		std::mt19937 g_Random { std::random_device {}() };

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
		T ReadAt(const uint8_t *Base, size_t Offset)
		{
			T value {};
			SafeCopy(&value, Base + Offset, sizeof(T));
			return value;
		}

		template<typename T>
		void WriteAt(uint8_t *Base, size_t Offset, const T& Value)
		{
			SafeCopy(Base + Offset, &Value, sizeof(T));
		}

		bool ReadArray(const uint8_t *Base, size_t Offset, RawArray& Out)
		{
			if (!SafeCopy(&Out, Base + Offset, sizeof(Out)))
				return false;

			return Out.m_Count > 0 && Out.m_Count <= MaxLocations && Out.m_Data;
		}

		void *Link(const uint8_t *Base, size_t Offset)
		{
			return ReadAt<void *>(Base, Offset);
		}

		struct FactContext
		{
			alignas(16) uint8_t m_Data[16] = {};

			explicit FactContext(const uint8_t *Base)
			{
				SafeCopy(m_Data, Base + Instance::Context, sizeof(m_Data));
			}

			void *Get()
			{
				return m_Data;
			}
		};

		bool MarkerPosition(void *Marker, double Out[3])
		{
			if (!Marker)
				return false;

			static const auto native = []() -> MarkerPositionFn
			{
				const auto module = GetModuleHandleW(L"fullgame.dll");
				const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

				if (!module)
					return nullptr;

				const auto fn = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(module) + SlotMarkerPosition);
				const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(exe + reinterpret_cast<const IMAGE_DOS_HEADER *>(exe)->e_lfanew);

				if (fn < exe || fn >= exe + nt->OptionalHeader.SizeOfImage)
					return nullptr;

				return reinterpret_cast<MarkerPositionFn>(fn);
			}();

			if (native)
			{
				alignas(16) double buffer[4] = {};
				native(buffer, Marker);
				std::copy_n(buffer, 3, Out);
				return true;
			}

			// Fallback: WorldNode::Orientation
			WorldTransform transform {};
			auto object = static_cast<RTTIRefObject *>(Marker);

			if (!object->GetRTTI()->AsCompound()->GetMemberValue(object, "Orientation", &transform))
				return false;

			Out[0] = transform.Position.X;
			Out[1] = transform.Position.Y;
			Out[2] = transform.Position.Z;
			return true;
		}

		std::vector<LocationData> ReadLocations(const uint8_t *Base)
		{
			std::vector<LocationData> result;
			RawArray markers {};

			if (!ReadArray(Base, Instance::Locations, markers))
				return result;

			result.resize(markers.m_Count);

			for (int i = 0; i < markers.m_Count; i++)
			{
				auto& l = result[i];
				SafeCopy(&l.m_Marker, markers.m_Data + i * sizeof(void *), sizeof(void *));
				l.m_HavePosition = MarkerPosition(l.m_Marker, l.m_Position);
			}

			auto readEach = [&](size_t Offset, size_t Stride, auto Apply)
			{
				RawArray a {};

				if (!ReadArray(Base, Offset, a))
					return;

				for (int i = 0; i < std::min(a.m_Count, markers.m_Count); i++)
					Apply(result[i], a.m_Data + i * Stride);
			};

			readEach(Instance::Variants, 4, [](auto& L, auto P) { SafeCopy(&L.m_Variant, P, 4); });
			readEach(Instance::Rotations, 4, [](auto& L, auto P) { SafeCopy(&L.m_Rotation, P, 4); });
			readEach(Instance::BlendRadii, 4, [](auto& L, auto P) { SafeCopy(&L.m_BlendRadius, P, 4); });
			readEach(Instance::Sizes, 16, [](auto& L, auto P)
			{
				SafeCopy(&L.m_Inner, P, 4);
				SafeCopy(&L.m_Outer, P + 4, 4);
			});
			readEach(Instance::Flips, 1, [](auto& L, auto P) { SafeCopy(&L.m_Flip, P, 1); });

			return result;
		}

		void *BlockFactFor(const uint8_t *Base, int Index)
		{
			switch (Index)
			{
			case 0:
				return Link(Base, Instance::BlockFact0);
			case 2:
				return Link(Base, Instance::BlockFact2);
			case 3:
				return Link(Base, Instance::BlockFact3);
			}

			return nullptr;
		}

		bool IsBlocked(uint8_t *Base, int Index, std::string *BlockedBy = nullptr)
		{
			auto fact = BlockFactFor(Base, Index);

			if (!fact)
				return false;

			FactContext ctx(Base);
			GGUUID uuid {};

			if (BlockedBy && GetObjectUUID(fact, &uuid))
				*BlockedBy = GetFactLabel(uuid);

			return ReadEnum(ctx.Get(), fact) == Link(Base, Instance::InProgressEntry);
		}

		void UpdateQuestBlockOverrides(const uint8_t *Base, bool Ignore)
		{
			if (!Ignore)
			{
				RemoveOverrides("supercell-quest-blocks");
				return;
			}

			std::vector<FactOverride> overrides;

			for (int index : { 0, 2, 3 })
			{
				FactOverride o;
				o.m_Graphs = { ContainerTag };
				o.m_Value.m_Kind = FactKind::Enum;
				o.m_Value.m_OnlyWhenEntry = QuestInProgress; // Report "not started" only while it would block

				if (GetObjectUUID(BlockFactFor(Base, index), &o.m_Fact))
					overrides.emplace_back(std::move(o));
			}

			SetOverrides("supercell-quest-blocks", std::move(overrides));
		}

		void UpdateSnapshot(uint8_t *Base)
		{
			const auto now = GetTickCount64();

			if (now - g_SnapshotTick < 500)
				return;

			g_SnapshotTick = now;

			const auto locations = ReadLocations(Base);
			std::vector<GameLocation> snapshot;

			{
				std::scoped_lock lock(g_SelectMutex);
				g_LocationCount = static_cast<int>(locations.size());
			}

			// Name the quest facts that block locations
			for (int index : { 0, 2, 3 })
			{
				GGUUID uuid {};

				if (GetObjectUUID(BlockFactFor(Base, index), &uuid) && GetFactLabel(uuid) == std::format("{}", uuid))
					SetFactLabel(uuid, std::format("Quest blocking supercell location {}", index));
			}

			std::vector<bool> enabled;
			bool lastEnabled;
			{
				std::scoped_lock lock(g_SelectMutex);
				enabled = g_GameEnabled;
				lastEnabled = g_LastEnabled;
			}

			for (int i = 0; i < static_cast<int>(locations.size()); i++)
			{
				const auto& l = locations[i];
				GameLocation g;
				g.m_Index = i;
				g.m_RandomByGame = i < static_cast<int>(locations.size()) - 1;
				g.m_Enabled = g.m_RandomByGame ? enabled[i] : lastEnabled;
				g.m_HavePosition = l.m_HavePosition;
				std::copy_n(l.m_Position, 3, g.m_Position);
				g.m_Inner = l.m_Inner;
				g.m_Outer = l.m_Outer;
				g.m_BlendRadius = l.m_BlendRadius;
				g.m_Rotation = l.m_Rotation;
				g.m_Variant = l.m_Variant;
				g.m_Flip = l.m_Flip;
				g.m_Blocked = IsBlocked(Base, i, &g.m_BlockedBy);
				snapshot.emplace_back(std::move(g));
			}

			std::scoped_lock lock(g_SnapshotMutex);
			g_Snapshot = std::move(snapshot);
		}

		void WriteLocationLook(uint8_t *Base, const LocationData& L)
		{
			FactContext ctx(Base);
			WriteFloat(ctx.Get(), Link(Base, Instance::RotationLink), L.m_Rotation);
			WriteInt(ctx.Get(), Link(Base, Instance::VariantLink), L.m_Variant);
			WriteFloat(ctx.Get(), Link(Base, Instance::BlendRadiusLink), L.m_BlendRadius);
			WriteBool(ctx.Get(), Link(Base, Instance::FlipLink), L.m_Flip);
		}

		void WriteSizes(uint8_t *Base, float Inner, float Outer)
		{
			FactContext ctx(Base);
			WriteFloat(ctx.Get(), Link(Base, Instance::InnerLink), Inner);
			WriteFloat(ctx.Get(), Link(Base, Instance::OuterLink), Outer);
		}

		// Runs right after the container picked a location (entered the forming state)
		void OnLocationChosen(uint8_t *Base)
		{
			const auto locations = ReadLocations(Base);
			const int count = static_cast<int>(locations.size());
			const int gameIndex = ReadAt<int>(Base, Instance::ChosenIndex);

			std::unique_lock lock(g_SelectMutex);
			g_ActiveCustom.reset();
			g_CustomIndex = -1;

			std::optional<CustomLocation> point;
			int pick = -1;

			auto isEnabled = [&](int Index) { return Index == count - 1 ? g_LastEnabled : g_GameEnabled[Index]; };
			bool select = g_Settings.m_UseCustom;

			for (int i = 0; i < std::min(count, MaxLocations); i++)
				select |= isEnabled(i) != (i != count - 1);

			if (!select || count == 0)
			{
				g_LastPick = gameIndex;
				return;
			}

			{
				auto collect = [&](bool AllowRepeat)
				{
					std::vector<int> candidates;

					for (int i = 0; i < std::min(count, MaxLocations); i++)
					{
						if (!isEnabled(i) || (!AllowRepeat && i == g_LastPick))
							continue;

						if (!g_Settings.m_IgnoreQuestBlocks && IsBlocked(Base, i))
							continue;

						candidates.emplace_back(i);
					}

					if (g_Settings.m_UseCustom)
					{
						for (int i = 0; i < static_cast<int>(g_Custom.size()); i++)
						{
							if (g_Custom[i].m_Enabled && (AllowRepeat || -2 - i != g_LastPick))
								candidates.emplace_back(-2 - i);
						}
					}

					return candidates;
				};

				auto candidates = collect(false);

				if (candidates.empty())
					candidates = collect(true);

				if (candidates.empty())
				{
					g_LastPick = gameIndex;
					return; // Nothing allowed, keep the game's choice
				}

				pick = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(g_Random)];
			}

			if (!point && pick <= -2)
				point = g_Custom[-2 - pick];

			if (!point && pick >= count)
			{
				g_LastPick = gameIndex;
				return;
			}

			FactContext ctx(Base);

			if (!point)
			{
				// One of the game's own locations
				const auto& l = locations[pick];

				if (l.m_HavePosition)
					WritePosition(ctx.Get(), Link(Base, Instance::PositionLink), l.m_Position);

				WriteLocationLook(Base, l);
				WriteSizes(Base, l.m_Inner, l.m_Outer);
				WriteAt<int>(Base, Instance::ChosenIndex, pick);
				WriteAt<int>(Base, Instance::LastIndex, pick);

				g_LastPick = pick;
				g_GameIndex = pick;
				spdlog::info("[supercells] storm placed at game location {}", pick);
				return;
			}

			// Custom point: copy the look of a template location
			int templateIndex = point->m_Template;

			if (templateIndex < 0 || templateIndex >= count)
				templateIndex = std::uniform_int_distribution<int>(0, count - 1)(g_Random);

			const auto& t = locations[templateIndex];
			double position[3] = { point->m_Position[0], point->m_Position[1], point->m_Position[2] };

			if (point->m_KeepGameAltitude && t.m_HavePosition)
				position[2] = t.m_Position[2];

			WritePosition(ctx.Get(), Link(Base, Instance::PositionLink), position);
			WriteLocationLook(Base, t);
			WriteSizes(Base, point->m_Inner > 0 ? point->m_Inner : t.m_Inner, point->m_Outer > 0 ? point->m_Outer : t.m_Outer);

			// The container indexes its arrays with these, keep them valid
			WriteAt<int>(Base, Instance::ChosenIndex, templateIndex);
			WriteAt<int>(Base, Instance::LastIndex, templateIndex);

			g_LastPick = pick;
			g_GameIndex = templateIndex;
			g_CustomIndex = pick <= -2 ? -2 - pick : -1;
			g_ActiveCustom = point;
			std::copy_n(position, 3, g_ActivePosition);

			spdlog::info("[supercells] storm placed at {:.0f}, {:.0f}, {:.0f} (look of location {})", position[0], position[1], position[2], templateIndex);
		}

		// The container re-applies its location on some transitions; keep a custom point in place
		void KeepCustomLocation(uint8_t *Base, int State)
		{
			std::scoped_lock lock(g_SelectMutex);

			if (!g_ActiveCustom)
				return;

			if (State != StateForming && State != StateActive)
			{
				g_ActiveCustom.reset();
				g_CustomIndex = -1;
				return;
			}

			FactContext ctx(Base);
			auto fact = Link(Base, Instance::PositionLink);
			double current[3];

			if (ReadPosition(ctx.Get(), fact, current) &&
				(std::abs(current[0] - g_ActivePosition[0]) > 1.0 || std::abs(current[1] - g_ActivePosition[1]) > 1.0))
				WritePosition(ctx.Get(), fact, g_ActivePosition);
		}

		std::vector<std::string> StormGraphs()
		{
			return { LimiterTag, WeatherTag };
		}

		void ApplyPositionOverride(const double Position[3], bool KeepAltitude)
		{
			FactOverride o;
			o.m_Graphs = StormGraphs();
			o.m_Fact = PositionFact;
			o.m_Value.m_Kind = FactKind::Position;
			std::copy_n(Position, 3, o.m_Value.m_Position);
			o.m_Value.m_PositionAxes[2] = !KeepAltitude;

			SetOverrides("supercell-position", { o });
		}

		void RefreshPlayerOverride()
		{
			std::scoped_lock lock(g_PositionMutex);

			if (g_PositionMode != PositionMode::Player)
				return;

			double position[3];

			if (!GetPlayerPosition(position))
				return;

			// Avoid rebuilding the override list every frame while standing still
			if (std::abs(position[0] - g_LastPlayerOverride[0]) + std::abs(position[1] - g_LastPlayerOverride[1]) < 1.0)
				return;

			std::copy_n(position, 3, g_LastPlayerOverride);
			ApplyPositionOverride(position, g_KeepAltitude);
		}

		uint64_t HookSceneUpdate(void *A, void *B, void *State, void *InstancePtr, void *E, void *F)
		{
			const auto base = static_cast<uint8_t *>(State);
			const auto instance = static_cast<uint8_t *>(InstancePtr);
			const int before = *reinterpret_cast<int *>(base + StateOffset);

			g_Instance = instance;

			if (before == StateActive)
			{
				auto current = reinterpret_cast<float *>(base + ActiveTimer);
				auto end = reinterpret_cast<float *>(base + ActiveTimer + 4);

				if (g_Hold)
				{
					// Freeze the timer where it was when hold was switched on
					if (g_HeldTime < 0.0f)
						g_HeldTime = std::min(*current, std::max(*end - 5.0f, 0.0f));

					*current = g_HeldTime;
				}
				else
				{
					g_HeldTime = -1.0f;
				}
			}
			else
			{
				g_HeldTime = -1.0f;
			}

			RefreshPlayerOverride();

			const auto result = g_OriginalSceneUpdate(A, B, State, InstancePtr, E, F);
			int after = *reinterpret_cast<int *>(base + StateOffset);

			// The merge quest switches the running container off directly, the fact overrides only cover later loads.
			// Restart it the way a save load does.
			if (after == StateDisabled && ModConfiguration.KeepSupercellsAfterAether)
			{
				static unsigned long long lastLog = 0;

				if (const auto now = GetTickCount64(); now - lastLog > 10000)
				{
					lastLog = now;
					spdlog::info("[supercells] container was disabled (state {} -> {}), restarting it", before, after);
				}

				*reinterpret_cast<int *>(base + StateOffset) = 0;
				after = 0;
			}

			if (after == StateForming && before != StateForming)
				OnLocationChosen(instance);

			KeepCustomLocation(instance, after);

			bool blocksDirty;
			bool ignoreBlocks;
			{
				std::scoped_lock lock(g_SelectMutex);
				blocksDirty = std::exchange(g_BlockOverridesDirty, false);
				ignoreBlocks = g_Settings.m_IgnoreQuestBlocks;
			}

			if (blocksDirty)
				UpdateQuestBlockOverrides(instance, ignoreBlocks);

			UpdateSnapshot(instance);

			const size_t timer = after == StateActive ? ActiveTimer : after == StateForming ? FormingTimer : 0;

			g_State = after;
			g_GameIndex = ReadAt<int>(instance, Instance::ChosenIndex);
			g_TimerCurrent = timer ? *reinterpret_cast<float *>(base + timer) : 0.0f;
			g_TimerEnd = timer ? *reinterpret_cast<float *>(base + timer + 4) : 0.0f;
			g_LastUpdateTick = GetTickCount64();

			return result;
		}
	}

	void Initialize()
	{
		SetFactLabel(GGUUID::Parse("E5E98601-17CB-644B-975B-E1EB6CB9571C"), "Supercells disabled (AETHER merged)");
		SetFactLabel(GGUUID::Parse("5FB621F0-9A0E-3040-9D75-7D48CF594FAB"), "Supercell weather enabled");
		SetFactLabel(GGUUID::Parse("095C0A02-BBF4-CB40-9E67-85E355006361"), "Post-merge lightning switch");
		SetFactLabel(GGUUID::Parse("05D97477-7F4F-F94E-9D85-F5954FC042B9"), "Late main quest state");
		SetFactLabel(PositionFact, "Supercell position");
		SetFactLabel(GGUUID::Parse("381695E6-D0D7-B749-B3BA-DB3063B5ECA6"), "Supercell rotation");
		SetFactLabel(GGUUID::Parse("11B7A0B4-20C9-A444-83F8-DEFB65EA82F9"), "Supercell variant");
		SetFactLabel(GGUUID::Parse("BB7E8786-F350-ED44-B7FB-63E4715206A9"), "Supercell weather blend radius");
		SetFactLabel(GGUUID::Parse("49F66DB5-1164-F848-A525-C445BF1929BE"), "Supercell inner radius");
		SetFactLabel(GGUUID::Parse("EE3D9BE4-EC4D-D34B-B77F-7B756FBFBC20"), "Supercell outer radius");
		SetFactLabel(GGUUID::Parse("A0B8D2AD-4009-B545-B790-816EFD98CA55"), "Supercell stormback flip");
		SetFactLabel(GGUUID::Parse("FF6DACB9-2561-1545-82C4-4744E2B58B2B"), "Storm up");
		SetFactLabel(GGUUID::Parse("3970E323-EA24-2F41-BD95-8E72A5E56000"), "Weather: supercell active");
		SetFactLabel(GGUUID::Parse("3AE564BB-F7BC-A344-AAA5-D9A866EFF592"), "Storm intensity");
		SetFactLabel(GGUUID::Parse("4E06708B-711A-BD43-BF8B-EC33FF8AE870"), "Lightning sequence step");

		{
			std::scoped_lock lock(g_SelectMutex);
			g_LastEnabled = ModConfiguration.SupercellIncludeAllLocations;
			g_Settings.m_IgnoreQuestBlocks = ModConfiguration.SupercellIgnoreQuestBlocks;
			g_Settings.m_UseCustom = ModConfiguration.SupercellUseCustomLocations;

			for (int index : ModConfiguration.SupercellDisabledLocations)
			{
				if (index >= 0 && index < MaxLocations)
					g_GameEnabled[index] = false;
			}

			for (const auto& l : ModConfiguration.SupercellLocations)
			{
				CustomLocation c;
				c.m_Enabled = l.Enabled;
				c.m_Position[0] = l.X;
				c.m_Position[1] = l.Y;
				c.m_Position[2] = l.Z;
				c.m_KeepGameAltitude = !l.HasZ;
				c.m_Inner = l.Inner;
				c.m_Outer = l.Outer;
				c.m_Template = l.Template;
				g_Custom.emplace_back(c);
			}
		}

		const bool trace = ModConfiguration.TraceSupercells;
		WatchGraph(ContainerTag, trace);
		WatchGraph(LimiterTag, trace);
		WatchGraph(WeatherTag, trace);

		if (!ModConfiguration.KeepSupercellsAfterAether)
			return;

		// Make the supercell scripts see a pre-merge world: these were flipped by the AETHER merge and the late quests
		const std::vector<std::string> graphs { ContainerTag, LimiterTag, WeatherTag };
		std::vector<FactOverride> overrides;

		auto add = [&](const char *Fact, FactKind Kind, const char *Value)
		{
			FactOverride o;
			o.m_Graphs = graphs;
			o.m_Fact = GGUUID::Parse(std::string_view(Fact));
			o.m_Value = ParseOverrideValue(Kind, Value).value();
			overrides.emplace_back(std::move(o));
		};

		add("E5E98601-17CB-644B-975B-E1EB6CB9571C", FactKind::Bool, "false");
		add("5FB621F0-9A0E-3040-9D75-7D48CF594FAB", FactKind::Bool, "true");
		add("095C0A02-BBF4-CB40-9E67-85E355006361", FactKind::Bool, "false");
		add("05D97477-7F4F-F94E-9D85-F5954FC042B9", FactKind::Enum, "default");

		SetOverrides("keep-supercells", std::move(overrides));
	}

	void TryInstall()
	{
		if (g_Installed.load() || !IsInstalled())
			return;

		auto target = FindEntryPoint(SceneUpdateEntry);
		g_Installed = true;

		if (!target)
		{
			spdlog::warn("[supercells] container update entry point not found, storm controls disabled");
			return;
		}

		g_OriginalSceneUpdate = reinterpret_cast<HandlerFn>(target);

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<void **>(&g_OriginalSceneUpdate), reinterpret_cast<void *>(&HookSceneUpdate));

		if (DetourTransactionCommit() != NO_ERROR)
		{
			g_OriginalSceneUpdate = nullptr;
			spdlog::error("[supercells] failed to hook the container update");
		}
	}

	Status GetStatus()
	{
		Status s;
		s.m_GraphHooks = IsInstalled();
		s.m_ContainerHooked = g_OriginalSceneUpdate != nullptr;
		s.m_HaveInstance = g_Instance.load() != nullptr;
		s.m_State = g_State;
		s.m_TimerCurrent = g_TimerCurrent;
		s.m_TimerEnd = g_TimerEnd;
		s.m_LastUpdateTick = g_LastUpdateTick;
		s.m_GameIndex = g_GameIndex;
		s.m_CustomIndex = g_CustomIndex;
		return s;
	}

	const char *StateName(int State)
	{
		switch (State)
		{
		case -1:
			return "not running";
		case 0:
			return "starting";
		case StateChoose:
			return "choosing location";
		case 2:
			return "rising";
		case StateRetryWait:
			return "waiting to retry a location";
		case 4:
			return "ending";
		case StateDissipate:
			return "dissipating";
		case StateForming:
			return "forming";
		case StateActive:
			return "active";
		case StateDisabled:
			return "disabled (AETHER merged)";
		}

		return "other";
	}

	void SetHoldStorm(bool Hold)
	{
		g_Hold = Hold;
	}

	bool GetHoldStorm()
	{
		return g_Hold;
	}

	SelectionSettings GetSelectionSettings()
	{
		std::scoped_lock lock(g_SelectMutex);
		return g_Settings;
	}

	void SetSelectionSettings(const SelectionSettings& Settings)
	{
		std::scoped_lock lock(g_SelectMutex);
		g_BlockOverridesDirty |= Settings.m_IgnoreQuestBlocks != g_Settings.m_IgnoreQuestBlocks;
		g_Settings = Settings;
	}

	std::vector<GameLocation> GetGameLocations()
	{
		std::scoped_lock lock(g_SnapshotMutex);
		return g_Snapshot;
	}

	void SetGameLocationEnabled(int Index, bool Enabled)
	{
		if (Index < 0 || Index >= MaxLocations)
			return;

		{
			std::scoped_lock lock(g_SelectMutex);

			if (Index == g_LocationCount - 1)
				g_LastEnabled = Enabled;
			else
				g_GameEnabled[Index] = Enabled;
		}

		std::scoped_lock lock(g_SnapshotMutex);

		for (auto& l : g_Snapshot)
		{
			if (l.m_Index == Index)
				l.m_Enabled = Enabled;
		}
	}

	std::vector<CustomLocation> GetCustomLocations()
	{
		std::scoped_lock lock(g_SelectMutex);
		return g_Custom;
	}

	void SetCustomLocationEnabled(size_t Index, bool Enabled)
	{
		std::scoped_lock lock(g_SelectMutex);

		if (Index < g_Custom.size())
			g_Custom[Index].m_Enabled = Enabled;
	}

	void SetPositionOverride(PositionMode Mode, const double Position[3], bool KeepAltitude)
	{
		std::scoped_lock lock(g_PositionMutex);

		g_PositionMode = Mode;
		g_KeepAltitude = KeepAltitude;
		std::fill_n(g_LastPlayerOverride, 3, 1e30);

		double player[3];

		if (Mode == PositionMode::Off)
			RemoveOverrides("supercell-position");
		else if (Mode == PositionMode::Fixed && Position)
			ApplyPositionOverride(Position, KeepAltitude);
		else if (Mode == PositionMode::Player && GetPlayerPosition(player))
			ApplyPositionOverride(player, KeepAltitude);
	}

	PositionMode GetPositionMode()
	{
		std::scoped_lock lock(g_PositionMutex);
		return g_PositionMode;
	}

	bool GetPlayerPosition(double Out[3])
	{
		auto player = Player::GetLocalPlayer();

		if (!player || !player->m_Entity)
			return false;

		const auto position = player->m_Entity->GetWorldTransform().Position;
		Out[0] = position.X;
		Out[1] = position.Y;
		Out[2] = position.Z;
		return true;
	}
}
