#pragma once

#include <imgui.h>
#include "../GraphRuntime.h"
#include "DebugUIWindow.h"

namespace HRZ2::DebugUI
{
	// Generic view of the facts that watched graph programs read and write, with live overrides
	class GraphFactsWindow : public Window
	{
	private:
		bool m_WindowOpen = true;

		static inline char m_NewTag[256] = {};
		static inline bool m_NewTagTrace = false;
		static inline char m_NewFact[64] = {};
		static inline ImGuiTextFilter m_Filter;

		static inline GraphRuntime::ObservedFact m_EditFact;
		static inline char m_EditValue[128] = {};
		static inline bool m_EditAllGraphs = false;

	public:
		virtual void Render() override;
		virtual bool Close() override;
		virtual std::string GetId() const override;
	};
}
