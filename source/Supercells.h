#pragma once

// Supercell storm control built on GraphRuntime
namespace Supercells
{
	enum class PositionMode : int
	{
		Off,
		Player,
		Fixed,
	};

	struct Status
	{
		bool m_GraphHooks = false;
		bool m_ContainerHooked = false;
		int m_State = -1;
		float m_TimerCurrent = 0.0f;
		float m_TimerEnd = 0.0f;
		unsigned long long m_LastUpdateTick = 0;
	};

	void Initialize();
	void TryInstall();

	Status GetStatus();
	const char *StateName(int State);

	void RequestNextStorm();
	void SetHoldStorm(bool Hold);
	bool GetHoldStorm();

	void SetPositionOverride(PositionMode Mode, const double Position[3], bool KeepAltitude);
	PositionMode GetPositionMode();

	void SetSizeOverride(bool Enable, float Inner, float Outer);
	bool GetPlayerPosition(double Out[3]);
}
