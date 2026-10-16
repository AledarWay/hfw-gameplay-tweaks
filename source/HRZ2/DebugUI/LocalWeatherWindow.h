#pragma once

#include <unordered_map>
#include "DebugUIWindow.h"

namespace HRZ2
{
	class RTTIRefObject;
}

namespace HRZ2::DebugUI
{
	// Research window: lists runtime LocalWeather objects (supercells, storm clouds, regular local weather)
	class LocalWeatherWindow : public Window
	{
	private:
		struct Snapshot
		{
			bool m_Enabled;
			float m_BlendFactor;
		};

		bool m_WindowOpen = true;

		static inline bool m_ForceSupercells = false;
		static inline bool m_LogChanges = true;
		static inline std::unordered_map<RTTIRefObject *, Snapshot> m_LastState;

	public:
		virtual void Render() override;
		virtual bool Close() override;
		virtual std::string GetId() const override;
	};
}
