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
// Graphs are selected by "tags": substrings of their entry point names (see Scan Graph Programs).
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
		bool m_LastWasWrite = false;
		bool m_Overridden = false;
		std::string m_Value;
		double m_Position[3] = {};
		float m_Number = 0.0f;
		uint64_t m_Count = 0;
		uint64_t m_LastTick = 0;
	};

	struct WatchedGraph
	{
		std::string m_Tag;
		bool m_Trace = false;
		size_t m_EntryPoints = 0;
		uintptr_t m_Begin = 0;
		uintptr_t m_End = 0;
	};

	// Setup
	void LoadConfig(); // [GraphDebug] and [[FactOverride]] from mod_config.ini
	void TryInstall();
	bool IsInstalled();
	void OnGraphProgramLoaded(RTTIRefObject *Graph);

	// Graph selection
	void WatchGraph(const std::string& Tag, bool Trace);
	void SetTrace(const std::string& Tag, bool Trace);
	std::vector<WatchedGraph> GetWatchedGraphs();
	void *FindEntryPoint(std::string_view Name);

	// Overrides
	void AddOverride(FactOverride Override);
	void SetOverrides(const std::string& Source, std::vector<FactOverride> Overrides);
	void RemoveOverrides(const std::string& Source);
	void SetOverrideEnabled(size_t Index, bool Enabled);
	void RemoveOverride(size_t Index);
	std::vector<FactOverride> GetOverrides();
	std::string OverrideToToml(const FactOverride& Override);

	// Observed values
	std::vector<ObservedFact> GetObservedFacts();
	std::optional<ObservedFact> GetObservedFact(const GGUUID& Fact);
	void ClearObservedFacts();

	// Readable names for facts in the debug UI
	void SetFactLabel(const GGUUID& Fact, std::string Label);
	std::string GetFactLabel(const GGUUID& Fact);

	const char *KindName(FactKind Kind);
	std::optional<OverrideValue> ParseOverrideValue(FactKind Kind, std::string_view Text);
}
