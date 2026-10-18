#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Research tools for the compiled HTN planner domains of flying mounts (Sunwing, Waterwing) in fullgame.dll.
// A domain is a code module plus a "pool" of cells built at load: symbols, predicate indices, numbers and code
// pointers. Plans read world state facts by predicate index; patching pool cells changes what methods check.
namespace Htn
{
	struct DomainStatus
	{
		std::string m_Name;
		bool m_Hooked = false;
		uintptr_t m_Pool = 0;
		size_t m_Agents = 0;
		size_t m_PatchesApplied = 0;
	};

	struct Patch
	{
		bool m_Enabled = true;
		std::string m_Domain;
		uint32_t m_Offset = 0; // Byte offset in the pool
		std::string m_Value;   // Number, "pred:<name>", "sym:<name>", "fail", "mark-mount", "fail-if-mount", "pass-if-mount", "mark-mount-in-water" or "fail-if-mount-in-water"
		std::string m_Label;   // Shown in the menu, empty for [[HTNPatch]] entries
		bool m_Applied = false;
		uint64_t m_Original = 0;
		int m_Slot = -1; // Conditional cell slot
		std::string m_Error;
	};

	void Initialize();
	void TryInstall();

	std::vector<DomainStatus> GetDomains();
	std::vector<Patch> GetPatches();
	void SetPatchEnabled(size_t Index, bool Enabled);

	// Logs the world state of every agent of these domains that plans during the next few seconds
	void RequestWorldStateDump(uint32_t Milliseconds = 3000);

	// Records which code cells (methods, subtasks, primitives) each agent runs during the next few seconds
	void RequestDecisionTrace(uint32_t Milliseconds = 8000);
}
