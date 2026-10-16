#include <format>
#include <shared_mutex>
#include <unordered_set>
#include <imgui.h>
#include "../../ModCoreEvents.h"
#include "../Core/RTTI.h"
#include "../Core/RTTIRefObject.h"
#include "LocalWeatherWindow.h"

namespace HRZ2::DebugUI
{
	namespace
	{
		// Field layout comes from the dumped RTTI (LocalWeather, size 112). Resolved by name, not by offset.
		struct LocalWeatherView
		{
			Ref<RTTIRefObject> *m_Setup;
			float *m_BlendFactor;
			float *m_Radius;
			float *m_StormbackRadius;
			float *m_Position; // Vec2
			float *m_Height;
			bool *m_Enabled;
			int *m_WeatherType; // ELocalWeatherType
		};

		std::optional<LocalWeatherView> ResolveView(RTTIRefObject *Object)
		{
			try
			{
				return LocalWeatherView {
					.m_Setup = &Object->GetMemberRefUnsafe<Ref<RTTIRefObject>>("WeatherSetup"),
					.m_BlendFactor = &Object->GetMemberRefUnsafe<float>("BlendFactor"),
					.m_Radius = &Object->GetMemberRefUnsafe<float>("Radius"),
					.m_StormbackRadius = &Object->GetMemberRefUnsafe<float>("StormbackRadius"),
					.m_Position = &Object->GetMemberRefUnsafe<float>("Position"),
					.m_Height = &Object->GetMemberRefUnsafe<float>("Height"),
					.m_Enabled = &Object->GetMemberRefUnsafe<bool>("Enabled"),
					.m_WeatherType = &Object->GetMemberRefUnsafe<int>("WeatherType"),
				};
			}
			catch (const std::exception&)
			{
				return std::nullopt;
			}
		}

		const char *WeatherTypeName(int Type)
		{
			switch (Type)
			{
			case 0:
				return "Regular";
			case 1:
				return "SuperCellStorm";
			case 2:
				return "StormCloud";
			}

			return "?";
		}

		// LocalWeather objects are cached directly and also collected from WeatherSystem::LocalWeather arrays
		std::vector<Ref<RTTIRefObject>> CollectLocalWeathers()
		{
			auto& events = ModCoreEvents::GetInstance();
			std::vector<Ref<RTTIRefObject>> result;
			std::unordered_set<RTTIRefObject *> seen;

			std::shared_lock lock(events.m_CachedDataMutex);

			for (auto obj : events.m_CachedLocalWeathers)
			{
				if (seen.emplace(obj).second)
					result.emplace_back(obj);
			}

			for (auto system : events.m_CachedWeatherSystems)
			{
				try
				{
					auto& list = system->GetMemberRefUnsafe<Array<Ref<RTTIRefObject>>>("LocalWeather");

					for (auto& entry : list)
					{
						if (entry.GetPtr() && seen.emplace(entry.GetPtr()).second)
							result.emplace_back(entry);
					}
				}
				catch (const std::exception&)
				{
				}
			}

			return result;
		}
	}

	void LocalWeatherWindow::Render()
	{
		auto& events = ModCoreEvents::GetInstance();
		const auto objects = CollectLocalWeathers();

		// Runs every frame while the window exists, even if collapsed
		for (auto& obj : objects)
		{
			const auto view = ResolveView(obj.GetPtr());

			if (!view)
				continue;

			if (m_ForceSupercells && *view->m_WeatherType == 1)
			{
				*view->m_Enabled = true;
				*view->m_BlendFactor = 1.0f;
			}

			const Snapshot now { *view->m_Enabled, *view->m_BlendFactor };
			auto [itr, inserted] = m_LastState.try_emplace(obj.GetPtr(), now);

			if (!inserted && (itr->second.m_Enabled != now.m_Enabled || itr->second.m_BlendFactor != now.m_BlendFactor))
			{
				if (m_LogChanges)
				{
					spdlog::info(
						"LocalWeather {} [{}] Enabled {} -> {}, BlendFactor {:.3f} -> {:.3f}",
						obj->m_UUID,
						WeatherTypeName(*view->m_WeatherType),
						itr->second.m_Enabled,
						now.m_Enabled,
						itr->second.m_BlendFactor,
						now.m_BlendFactor);
				}

				itr->second = now;
			}
		}

		ImGui::SetNextWindowSize(ImVec2(900, 450), ImGuiCond_FirstUseEver);

		if (!ImGui::Begin(GetId().c_str(), &m_WindowOpen))
		{
			ImGui::End();
			return;
		}

		ImGui::Text(
			"LocalWeather objects: %zu (WeatherSystem resources: %zu)",
			objects.size(),
			[&]
			{
				std::shared_lock lock(events.m_CachedDataMutex);
				return events.m_CachedWeatherSystems.size();
			}());

		ImGui::Checkbox("Force SuperCellStorm entries enabled", &m_ForceSupercells);
		ImGui::SameLine();
		ImGui::Checkbox("Log changes", &m_LogChanges);
		ImGui::SameLine();

		if (ImGui::Button("Dump to asset_dump.txt"))
		{
			for (auto& obj : objects)
				events.DumpObject(obj.GetPtr(), "localweather");

			std::vector<Ref<RTTIRefObject>> systems;
			{
				std::shared_lock lock(events.m_CachedDataMutex);
				systems.assign(events.m_CachedWeatherSystems.begin(), events.m_CachedWeatherSystems.end());
			}

			for (auto& s : systems)
				events.DumpObject(s.GetPtr(), "weathersystem");

			spdlog::info("Dumped {} LocalWeather and {} WeatherSystem objects", objects.size(), systems.size());
		}

		constexpr auto tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
									ImGuiTableFlags_Resizable;

		if (ImGui::BeginTable("##LocalWeather", 8, tableFlags))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("UUID");
			ImGui::TableSetupColumn("Type");
			ImGui::TableSetupColumn("Enabled");
			ImGui::TableSetupColumn("BlendFactor");
			ImGui::TableSetupColumn("Radius / Stormback");
			ImGui::TableSetupColumn("Position");
			ImGui::TableSetupColumn("Height");
			ImGui::TableSetupColumn("Setup UUID");
			ImGui::TableHeadersRow();

			for (auto& obj : objects)
			{
				const auto view = ResolveView(obj.GetPtr());

				if (!view)
					continue;

				ImGui::PushID(obj.GetPtr());
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(std::format("{}", obj->m_UUID).c_str());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(WeatherTypeName(*view->m_WeatherType));

				ImGui::TableNextColumn();
				ImGui::Checkbox("##enabled", view->m_Enabled);

				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::SliderFloat("##blend", view->m_BlendFactor, 0.0f, 1.0f);

				ImGui::TableNextColumn();
				ImGui::Text("%.0f / %.0f", *view->m_Radius, *view->m_StormbackRadius);

				ImGui::TableNextColumn();
				ImGui::Text("%.0f, %.0f", view->m_Position[0], view->m_Position[1]);

				ImGui::TableNextColumn();
				ImGui::Text("%.0f", *view->m_Height);

				ImGui::TableNextColumn();

				if (auto setup = view->m_Setup->GetPtr())
					ImGui::TextUnformatted(std::format("{}", setup->m_UUID).c_str());
				else
					ImGui::TextUnformatted("null");

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::End();
	}

	bool LocalWeatherWindow::Close()
	{
		return !m_WindowOpen;
	}

	std::string LocalWeatherWindow::GetId() const
	{
		return "Local Weather";
	}
}
