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

		void RenderParameters();
		void RenderLocations(const double *Player);
		void RenderOverrides(const double *Player);

	public:
		virtual void Render() override;
		virtual bool Close() override;
		virtual std::string GetId() const override;
	};
}
