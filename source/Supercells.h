#pragma once

#include <string>
#include <vector>

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
		bool m_HaveInstance = false;
		int m_State = -1;
		float m_TimerCurrent = 0.0f;
		float m_TimerEnd = 0.0f;
		unsigned long long m_LastUpdateTick = 0;
		int m_GameIndex = -1;	// Location index the container uses
		int m_CustomIndex = -1; // Custom location placed by the mod, -1 if none
	};

	struct GameLocation
	{
		int m_Index = 0;
		bool m_Enabled = true;
		bool m_HavePosition = false;
		double m_Position[3] = {};
		float m_Inner = 0.0f;
		float m_Outer = 0.0f;
		float m_BlendRadius = 0.0f;
		float m_Rotation = 0.0f;
		int m_Variant = 0;
		bool m_Flip = false;
		bool m_RandomByGame = true; // The game's roll never reaches the last location
		bool m_Blocked = false;
		std::string m_BlockedBy;
	};

	struct CustomLocation
	{
		bool m_Enabled = true;
		double m_Position[3] = {};
		bool m_KeepGameAltitude = true; // Use the template location's Z
		float m_Inner = 0.0f;			// 0 = template's size
		float m_Outer = 0.0f;
		int m_Template = -1;			// Game location whose cloud look is copied, -1 = random
	};

	struct SelectionSettings
	{
		bool m_IncludeAll = false;
		bool m_IgnoreQuestBlocks = false;
		bool m_UseCustom = false;
	};

	void Initialize();
	void TryInstall();

	Status GetStatus();
	const char *StateName(int State);

	struct LightningStatus
	{
		bool m_Known = false;	// The storm scripts have been seen using the lightning facts
		bool m_Enabled = false; // Lightning sequences may run (storm is up)
		int m_Step = 0;			// 0 = idle between strikes
	};

	void RequestNextStorm();
	void RemoveCurrentStorm(); // Ends the storm and pauses new ones
	void SetStormsPaused(bool Paused);
	bool GetStormsPaused();
	void TriggerLightning();
	LightningStatus GetLightningStatus();
	void SetHoldStorm(bool Hold);
	bool GetHoldStorm();

	// Location selection
	SelectionSettings GetSelectionSettings();
	void SetSelectionSettings(const SelectionSettings& Settings);
	std::vector<GameLocation> GetGameLocations();
	void SetGameLocationEnabled(int Index, bool Enabled);
	std::vector<CustomLocation> GetCustomLocations();
	void AddCustomLocation(const CustomLocation& Location);
	void RemoveCustomLocation(size_t Index);
	void SetCustomLocationEnabled(size_t Index, bool Enabled);
	std::string CustomLocationsToToml();
	void SpawnAtGameLocation(int Index);
	void SpawnAtCustomLocation(size_t Index);
	void SpawnAt(const double Position[3], bool KeepGameAltitude);

	// Overrides for what the storm scripts read
	void SetPositionOverride(PositionMode Mode, const double Position[3], bool KeepAltitude);
	PositionMode GetPositionMode();

	// Writes the size facts directly, 0 leaves that value to the game
	void SetSizeOverride(bool Enable, float Inner, float Outer, float BlendRadius);
	bool GetPlayerPosition(double Out[3]);
}
