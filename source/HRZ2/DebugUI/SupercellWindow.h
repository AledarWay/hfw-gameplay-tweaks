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
		static inline float m_BlendRadius = 0.0f;
		static inline float m_CustomInner = 0.0f;
		static inline float m_CustomOuter = 0.0f;
		static inline int m_CustomTemplate = -1;

		void RenderParameters(unsigned long long Now);
		void RenderLocations(const double *Player);
		void RenderOverrides(const double *Player);

	public:
		virtual void Render() override;
		virtual bool Close() override;
		virtual std::string GetId() const override;
	};
}
