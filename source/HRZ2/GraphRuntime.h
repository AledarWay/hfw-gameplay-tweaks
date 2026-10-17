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
		std::optional<GGUUID> m_OnlyWhenEntry; // Enum only: apply just while the real value is this entry
		double m_Position[3] = {};
		bool m_PositionAxes[3] = { true, true, true }; // Axes left false keep the game's value
		bool m_WriteBack = false; // Bool/Int/Float/Enum: also store the value in the real fact, so native code sees it
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
		uint64_t m_ChangedTick = 0;
		void *m_Object = nullptr; // Fact object, valid while its asset stays loaded
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

	// Facts watched for every caller: any graph program, and native game code when native hooks are on
	// ([GraphDebug] NativeFactHooks). Callers show up as entry point names or "native+0x<exe rva>".
	void WatchFact(const GGUUID& Fact);
	void UnwatchFact(const GGUUID& Fact);
	std::vector<GGUUID> GetWatchedFacts();
	bool NativeHooksInstalled();

	// Overrides. For watched facts, graph filters also match caller names ("native" matches native code).
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
	std::string DescribeEnumEntry(void *Entry);

	const char *KindName(FactKind Kind);
	std::optional<OverrideValue> ParseOverrideValue(FactKind Kind, std::string_view Text);
}
