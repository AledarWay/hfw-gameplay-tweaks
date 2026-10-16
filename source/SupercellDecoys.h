#pragma once

namespace HRZ2
{
	class RTTIRefObject;
}

// Experiment: replace facts that the supercell script reads with private constant copies
namespace SupercellDecoys
{
	void OnHolderLoaded(HRZ2::RTTIRefObject *Holder);
	void OnHolderUnloaded(HRZ2::RTTIRefObject *Holder);
	void ApplyPending();
	void ApplyNow();
}
