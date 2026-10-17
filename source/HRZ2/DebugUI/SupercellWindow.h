#pragma once

#include "DebugUIWindow.h"

namespace HRZ2::DebugUI
{
	class SupercellWindow : public Window
	{
	private:
		bool m_WindowOpen = true;

		static inline int m_PositionMode = 0;
		static inline double m_Position[3] = {};
		static inline bool m_KeepAltitude = true;
		static inline bool m_SizeOverride = false;
		static inline float m_InnerRadius = 550.0f;
		static inline float m_OuterRadius = 675.0f;

	public:
		virtual void Render() override;
		virtual bool Close() override;
		virtual std::string GetId() const override;
	};
}
