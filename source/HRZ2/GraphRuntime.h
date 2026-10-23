#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "PCore/Common.h"

namespace HRZ2
{
	class RTTIRefObject;
}

// Observes and overrides fact reads/writes made by compiled graph programs in fullgame.dll.
// Graphs are selected by "tags": substrings of their entry point names.
namespace HRZ2::GraphRuntime
{
	enum class FactKind : uint8_t
	{
		Bool,
		Enum,
		Int,
		Float,
		Position,
	};

	struct OverrideValue
	{
		FactKind m_Kind = FactKind::Bool;
		bool m_Bool = false;
		int m_Int = 0;
		float m_Float = 0.0f;
		std::optional<GGUUID> m_EnumEntry; // Empty means the fact's default entry
		std::optional<GGUUID> m_OnlyWhenEntry; // Enum only: apply just while the real value is this entry
		double m_Position[3] = {};
		bool m_PositionAxes[3] = { true, true, true }; // Axes left false keep the game's value
	};

	struct FactOverride
	{
		std::vector<std::string> m_Graphs; // Graph tag substrings, empty matches every watched graph
		GGUUID m_Fact {};
		OverrideValue m_Value;
		bool m_Enabled = true;
		std::string m_Source;
	};

	struct ObservedFact
	{
		std::string m_Graph;
		GGUUID m_Fact {};
		FactKind m_Kind = FactKind::Bool;
		bool m_Overridden = false;
		std::string m_Value;
		double m_Position[3] = {};
		float m_Number = 0.0f;
		uint64_t m_Count = 0;
		uint64_t m_LastTick = 0;
	};

	// Setup
	void LoadConfig();
	void TryInstall();
	bool IsInstalled();
	void OnGraphProgramLoaded(RTTIRefObject *Graph);

	// Graph selection
	void WatchGraph(const std::string& Tag);
	void *FindEntryPoint(std::string_view Name);

	// Overrides
	void SetOverrides(const std::string& Source, std::vector<FactOverride> Overrides);
	void RemoveOverrides(const std::string& Source);

	// Last value read or written by a watched graph
	std::optional<ObservedFact> GetObservedFact(const GGUUID& Fact);

	// Direct fact access without overrides, for mod code on the game thread. Context is the graph's 16-byte fact context.
	std::optional<bool> ReadBool(void *Context, void *Fact);
	void *ReadEnum(void *Context, void *Fact);
	std::optional<int> ReadInt(void *Context, void *Fact);
	std::optional<float> ReadFloat(void *Context, void *Fact);
	bool ReadPosition(void *Context, void *Fact, double Out[3]);
	bool WriteBool(void *Context, void *Fact, bool Value);
	bool WriteInt(void *Context, void *Fact, int Value);
	bool WriteFloat(void *Context, void *Fact, float Value);
	bool WritePosition(void *Context, void *Fact, const double Value[3]);
	bool GetObjectUUID(const void *Object, GGUUID *Out);

	std::optional<OverrideValue> ParseOverrideValue(FactKind Kind, std::string_view Text);
}
