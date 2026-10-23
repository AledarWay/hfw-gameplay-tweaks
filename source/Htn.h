#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Patches for the compiled HTN planner domains of flying mounts (Sunwing, Waterwing) in fullgame.dll.
// A domain is a code module plus a "pool" of cells built at load: symbols, predicate indices, numbers and code
// pointers. Plans read world state facts by predicate index; patching pool cells changes what methods do.
namespace Htn
{
	struct Patch
	{
		bool m_Enabled = true;
		std::string m_Domain;
		uint32_t m_Offset = 0; // Byte offset in the pool
		std::string m_Value;   // "fail", "mark-mount", "fail-if-mount", "pass-if-mount", "mark-mount-in-water", "fail-if-mount-in-water", "delay-if-mount" or "double:<x>"
		bool m_Applied = false;
		uint64_t m_Original = 0;
		int m_Slot = -1; // Conditional cell slot
		uint64_t m_Expected = 0; // When set, the patch applies only over this original value
		std::string m_Error;
	};

	void Initialize();
	void TryInstall();
}
