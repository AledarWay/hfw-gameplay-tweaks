#include <Windows.h>
#include <detours/detours.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include "HRZ2/Core/Entity.h"
#include "HRZ2/Core/Player.h"
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
		constexpr auto InnerRadiusFact = GGUUID::Parse("49F66DB5-1164-F848-A525-C445BF1929BE");
		constexpr auto OuterRadiusFact = GGUUID::Parse("EE3D9BE4-EC4D-D34B-B77F-7B756FBFBC20");

		// Container state machine (MsgSceneUpdate): state index at +0x28, per-state timers before it
		constexpr int StateForming = 6;
		constexpr int StateActive = 7;
		constexpr size_t StateOffset = 0x28;
		constexpr size_t FormingTimer = 0x14;
		constexpr size_t ActiveTimer = 0x1C;

		using HandlerFn = uint64_t (*)(void *, void *, void *, void *, void *, void *);

		HandlerFn g_OriginalSceneUpdate = nullptr;
		std::atomic_bool g_Installed = false;

		std::atomic_int g_State = -1;
		std::atomic<float> g_TimerCurrent = 0.0f;
		std::atomic<float> g_TimerEnd = 0.0f;
		std::atomic_uint64_t g_LastUpdateTick = 0;

		std::atomic_bool g_SkipRequested = false;
		std::atomic_bool g_Hold = false;

		std::mutex g_PositionMutex;
		PositionMode g_PositionMode = PositionMode::Off;
		bool g_KeepAltitude = true;
		double g_LastPlayerOverride[3] = {};

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

		uint64_t HookSceneUpdate(void *A, void *B, void *State, void *Instance, void *E, void *F)
		{
			const auto base = static_cast<uint8_t *>(State);
			const int state = *reinterpret_cast<int *>(base + StateOffset);

			if (state == StateActive)
			{
				auto current = reinterpret_cast<float *>(base + ActiveTimer);
				auto end = reinterpret_cast<float *>(base + ActiveTimer + 4);

				if (g_SkipRequested.exchange(false))
					*current = *end + 1.0f;
				else if (g_Hold)
					*current = 0.0f;
			}

			RefreshPlayerOverride();

			const auto result = g_OriginalSceneUpdate(A, B, State, Instance, E, F);
			const int after = *reinterpret_cast<int *>(base + StateOffset);
			const size_t timer = after == StateActive ? ActiveTimer : after == StateForming ? FormingTimer : 0;

			g_State = after;
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
		SetFactLabel(InnerRadiusFact, "Supercell inner radius");
		SetFactLabel(OuterRadiusFact, "Supercell outer radius");
		SetFactLabel(GGUUID::Parse("BB7E8786-F350-ED44-B7FB-63E4715206A9"), "Supercell rotation");
		SetFactLabel(GGUUID::Parse("11B7A0B4-20C9-A444-83F8-DEFB65EA82F9"), "Supercell variant");
		SetFactLabel(GGUUID::Parse("FF6DACB9-2561-1545-82C4-4744E2B58B2B"), "Storm up");
		SetFactLabel(GGUUID::Parse("3970E323-EA24-2F41-BD95-8E72A5E56000"), "Weather: supercell active");
		SetFactLabel(GGUUID::Parse("3AE564BB-F7BC-A344-AAA5-D9A866EFF592"), "Storm intensity");
		SetFactLabel(GGUUID::Parse("4E06708B-711A-BD43-BF8B-EC33FF8AE870"), "Lightning sequence step");

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
			o.m_Fact = GGUUID::Parse(Fact);
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
		s.m_State = g_State;
		s.m_TimerCurrent = g_TimerCurrent;
		s.m_TimerEnd = g_TimerEnd;
		s.m_LastUpdateTick = g_LastUpdateTick;
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
		case 3:
			return "waiting";
		case 5:
			return "choosing location";
		case StateForming:
			return "forming";
		case StateActive:
			return "active";
		case 11:
			return "disabled (AETHER merged)";
		}

		return "other";
	}

	void RequestNextStorm()
	{
		g_SkipRequested = true;
	}

	void SetHoldStorm(bool Hold)
	{
		g_Hold = Hold;
	}

	bool GetHoldStorm()
	{
		return g_Hold;
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
		else if (Mode == PositionMode::Fixed)
			ApplyPositionOverride(Position, KeepAltitude);
		else if (GetPlayerPosition(player))
			ApplyPositionOverride(player, KeepAltitude);
	}

	PositionMode GetPositionMode()
	{
		std::scoped_lock lock(g_PositionMutex);
		return g_PositionMode;
	}

	void SetSizeOverride(bool Enable, float Inner, float Outer)
	{
		if (!Enable)
		{
			RemoveOverrides("supercell-size");
			return;
		}

		FactOverride inner;
		inner.m_Graphs = StormGraphs();
		inner.m_Fact = InnerRadiusFact;
		inner.m_Value.m_Kind = FactKind::Float;
		inner.m_Value.m_Float = Inner;

		FactOverride outer = inner;
		outer.m_Fact = OuterRadiusFact;
		outer.m_Value.m_Float = Outer;

		SetOverrides("supercell-size", { inner, outer });
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
