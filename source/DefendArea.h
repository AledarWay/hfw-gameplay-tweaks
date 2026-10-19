#pragma once

// Removes the leash that keeps wild machines (and camp guards) at their spawn site: spawn commands give them a defend
// area and a navmesh area restriction the pathfinder won't leave.
namespace DefendArea
{
	void TryInstall();
}
