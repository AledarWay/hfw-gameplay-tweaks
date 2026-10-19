#pragma once

// Wild machines (and camp guards) are kept inside the defend area of their spawn site by the HTN planner daemon that
// publishes the agent's active defend area. Disabling its update leaves them without one.
namespace DefendArea
{
	void TryInstall();
}
