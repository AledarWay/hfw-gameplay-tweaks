#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <imgui.h>
#include "../../Supercells.h"
#include "../GraphRuntime.h"
#include "SupercellWindow.h"

namespace HRZ2::DebugUI
{
	namespace
	{
		struct ParamRow
		{
			const char *m_Name;
			GGUUID m_Fact;
		};

		const ParamRow Params[] = {
			{ "Position", GGUUID::Parse("9F73BE2D-14F3-A840-BD6A-172FF8623CEA") },
			{ "Inner radius", GGUUID::Parse("49F66DB5-1164-F848-A525-C445BF1929BE") },
			{ "Outer radius", GGUUID::Parse("EE3D9BE4-EC4D-D34B-B77F-7B756FBFBC20") },
			{ "Weather blend radius", GGUUID::Parse("BB7E8786-F350-ED44-B7FB-63E4715206A9") },
			{ "Rotation", GGUUID::Parse("381695E6-D0D7-B749-B3BA-DB3063B5ECA6") },
			{ "Variant", GGUUID::Parse("11B7A0B4-20C9-A444-83F8-DEFB65EA82F9") },
			{ "Stormback flip", GGUUID::Parse("A0B8D2AD-4009-B545-B790-816EFD98CA55") },
			{ "Storm up", GGUUID::Parse("FF6DACB9-2561-1545-82C4-4744E2B58B2B") },
			{ "Weather active", GGUUID::Parse("3970E323-EA24-2F41-BD95-8E72A5E56000") },
			{ "Intensity", GGUUID::Parse("3AE564BB-F7BC-A344-AAA5-D9A866EFF592") },
			{ "Lightning step", GGUUID::Parse("4E06708B-711A-BD43-BF8B-EC33FF8AE870") },
		};

		const ImVec4 Warning(1, 0.5f, 0.3f, 1);
		const ImVec4 Highlight(1, 0.8f, 0.2f, 1);
		const ImVec4 Active(0.4f, 1, 0.4f, 1);
	}

	void SupercellWindow::Render()
	{
		ImGui::SetNextWindowSize(ImVec2(760, 760), ImGuiCond_FirstUseEver);

		if (!ImGui::Begin(GetId().c_str(), &m_WindowOpen))
		{
			ImGui::End();
			return;
		}

		const auto status = Supercells::GetStatus();
		const auto now = GetTickCount64();

		if (!status.m_GraphHooks)
			ImGui::TextColored(Warning, "Graph hooks are not installed yet (load a save).");
		else if (!status.m_ContainerHooked)
			ImGui::TextColored(Warning, "Supercell container hook is missing, see mod_log.txt.");

		ImGui::Text("Container: %s (state %d)", Supercells::StateName(status.m_State), status.m_State);

		if (status.m_CustomIndex >= 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(Active, "at custom location %d", status.m_CustomIndex);
		}
		else if (status.m_GameIndex >= 0)
		{
			ImGui::SameLine();
			ImGui::TextColored(Active, "at game location %d", status.m_GameIndex);
		}

		if (status.m_TimerEnd > 0.0f)
		{
			ImGui::ProgressBar(
				std::clamp(status.m_TimerCurrent / status.m_TimerEnd, 0.0f, 1.0f),
				ImVec2(-FLT_MIN, 0),
				std::format("{:.0f} / {:.0f} s", status.m_TimerCurrent, status.m_TimerEnd).c_str());
		}

		if (ImGui::Button("Next storm now"))
			Supercells::RequestNextStorm();

		ImGui::SameLine();
		bool hold = Supercells::GetHoldStorm();

		if (ImGui::Checkbox("Hold current storm (freeze timer)", &hold))
			Supercells::SetHoldStorm(hold);

		double player[3] = {};
		const bool havePlayer = Supercells::GetPlayerPosition(player);

		if (havePlayer)
			ImGui::TextDisabled("Player: %.0f, %.0f, %.0f", player[0], player[1], player[2]);

		if (ImGui::CollapsingHeader("Current storm", ImGuiTreeNodeFlags_DefaultOpen))
			RenderParameters(now);

		if (ImGui::CollapsingHeader("Locations", ImGuiTreeNodeFlags_DefaultOpen))
			RenderLocations(havePlayer ? player : nullptr);

		if (ImGui::CollapsingHeader("Live overrides"))
			RenderOverrides(havePlayer ? player : nullptr);

		ImGui::End();
	}

	void SupercellWindow::RenderParameters(unsigned long long Now)
	{
		// Values as last read or written by the supercell scripts
		if (!ImGui::BeginTable("##StormParams", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			return;

		ImGui::TableSetupColumn("Parameter");
		ImGui::TableSetupColumn("Value");
		ImGui::TableSetupColumn("Last access");
		ImGui::TableSetupColumn("Changed");
		ImGui::TableHeadersRow();

		for (const auto& p : Params)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(p.m_Name);

			const auto fact = GraphRuntime::GetObservedFact(p.m_Fact);
			ImGui::TableNextColumn();

			if (!fact)
			{
				ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				continue;
			}

			if (fact->m_Overridden)
				ImGui::TextColored(Highlight, "%s *", fact->m_Value.c_str());
			else
				ImGui::TextUnformatted(fact->m_Value.c_str());

			ImGui::TableNextColumn();
			ImGui::Text("%s %s", fact->m_LastWasWrite ? "set by" : "read by", fact->m_Graph.substr(0, 24).c_str());

			ImGui::TableNextColumn();
			ImGui::Text("%.0fs ago", (Now - fact->m_ChangedTick) / 1000.0);
		}

		ImGui::EndTable();
		ImGui::TextDisabled("Values marked * are replaced by an override for the reading script.");
	}

	void SupercellWindow::RenderLocations(const double *Player)
	{
		auto settings = Supercells::GetSelectionSettings();
		bool changed = false;

		changed |= ImGui::Checkbox("Include the last game location in random picks", &settings.m_IncludeAll);
		ImGui::SameLine();
		ImGui::TextDisabled("(the game's roll never reaches it)");
		changed |= ImGui::Checkbox("Ignore quest blocks", &settings.m_IgnoreQuestBlocks);
		ImGui::SameLine();
		ImGui::TextDisabled("(some locations are skipped while certain quests are in progress)");
		changed |= ImGui::Checkbox("Use custom locations", &settings.m_UseCustom);

		if (changed)
			Supercells::SetSelectionSettings(settings);

		const auto status = Supercells::GetStatus();
		const auto locations = Supercells::GetGameLocations();

		ImGui::TextUnformatted("Game locations");

		if (locations.empty())
			ImGui::TextDisabled("Not read yet (needs a running supercell container).");
		else if (ImGui::BeginTable("##GameLocations", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Position");
			ImGui::TableSetupColumn("Inner / outer / blend");
			ImGui::TableSetupColumn("Variant", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Status");
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			for (const auto& l : locations)
			{
				ImGui::PushID(l.m_Index);
				ImGui::TableNextRow();

				ImGui::TableNextColumn();

				if (status.m_CustomIndex < 0 && status.m_GameIndex == l.m_Index)
					ImGui::TextColored(Active, "%d", l.m_Index);
				else
					ImGui::Text("%d", l.m_Index);

				ImGui::TableNextColumn();
				bool enabled = l.m_Enabled;

				if (ImGui::Checkbox("##use", &enabled))
					Supercells::SetGameLocationEnabled(l.m_Index, enabled);

				ImGui::TableNextColumn();

				if (l.m_HavePosition)
				{
					ImGui::Text("%.0f, %.0f, %.0f", l.m_Position[0], l.m_Position[1], l.m_Position[2]);

					if (Player)
					{
						const double dx = l.m_Position[0] - Player[0];
						const double dy = l.m_Position[1] - Player[1];
						ImGui::SameLine();
						ImGui::TextDisabled("(%.1f km)", std::sqrt(dx * dx + dy * dy) / 1000.0);
					}
				}
				else
				{
					ImGui::TextDisabled("not loaded");
				}

				ImGui::TableNextColumn();
				ImGui::Text("%.0f / %.0f / %.0f", l.m_Inner, l.m_Outer, l.m_BlendRadius);

				ImGui::TableNextColumn();
				ImGui::Text("%d%s", l.m_Variant, l.m_Flip ? " flip" : "");

				ImGui::TableNextColumn();

				if (l.m_Blocked)
					ImGui::TextColored(Warning, "blocked by quest");
				else if (!l.m_RandomByGame)
					ImGui::TextDisabled("not rolled by the game");
				else
					ImGui::TextDisabled("ok");

				if (!l.m_BlockedBy.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("Blocked while this quest is in progress:\n%s", l.m_BlockedBy.c_str());

				ImGui::TableNextColumn();

				if (ImGui::SmallButton("Storm here"))
					Supercells::SpawnAtGameLocation(l.m_Index);

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextUnformatted("Custom locations");
		const auto custom = Supercells::GetCustomLocations();

		if (!custom.empty() && ImGui::BeginTable("##CustomLocations", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Position");
			ImGui::TableSetupColumn("Inner / outer");
			ImGui::TableSetupColumn("Look of", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			for (size_t i = 0; i < custom.size(); i++)
			{
				const auto& c = custom[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();

				ImGui::TableNextColumn();

				if (status.m_CustomIndex == static_cast<int>(i))
					ImGui::TextColored(Active, "%zu", i);
				else
					ImGui::Text("%zu", i);

				ImGui::TableNextColumn();
				bool enabled = c.m_Enabled;

				if (ImGui::Checkbox("##use", &enabled))
					Supercells::SetCustomLocationEnabled(i, enabled);

				ImGui::TableNextColumn();

				if (c.m_KeepGameAltitude)
					ImGui::Text("%.0f, %.0f, (game altitude)", c.m_Position[0], c.m_Position[1]);
				else
					ImGui::Text("%.0f, %.0f, %.0f", c.m_Position[0], c.m_Position[1], c.m_Position[2]);

				ImGui::TableNextColumn();

				if (c.m_Inner > 0 || c.m_Outer > 0)
					ImGui::Text("%.0f / %.0f", c.m_Inner, c.m_Outer);
				else
					ImGui::TextDisabled("from look");

				ImGui::TableNextColumn();

				if (c.m_Template >= 0)
					ImGui::Text("%d", c.m_Template);
				else
					ImGui::TextDisabled("random");

				ImGui::TableNextColumn();

				if (ImGui::SmallButton("Storm here"))
					Supercells::SpawnAtCustomLocation(i);

				ImGui::SameLine();

				if (ImGui::SmallButton("Remove"))
					Supercells::RemoveCustomLocation(i);

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::SetNextItemWidth(160);
		ImGui::InputFloat("Inner##new", &m_CustomInner, 0, 0, "%.0f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160);
		ImGui::InputFloat("Outer##new", &m_CustomOuter, 0, 0, "%.0f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120);
		ImGui::InputInt("Look of##new", &m_CustomTemplate);
		m_CustomTemplate = std::clamp(m_CustomTemplate, -1, 15);
		ImGui::TextDisabled("0 = size of the copied look, look of -1 = random game location");

		ImGui::Checkbox("Keep game altitude (ignore Z)", &m_KeepAltitude);

		if (ImGui::Button("Add player position") && Player)
		{
			Supercells::CustomLocation c;
			std::copy_n(Player, 3, c.m_Position);
			c.m_KeepGameAltitude = m_KeepAltitude;
			c.m_Inner = m_CustomInner;
			c.m_Outer = m_CustomOuter;
			c.m_Template = m_CustomTemplate;
			Supercells::AddCustomLocation(c);
		}

		ImGui::SameLine();

		if (ImGui::Button("Storm above player") && Player)
			Supercells::SpawnAt(Player, m_KeepAltitude);

		ImGui::SameLine();

		if (ImGui::Button("Print custom locations to log as config"))
			spdlog::info("\n{}", Supercells::CustomLocationsToToml());

		ImGui::TextDisabled("\"Storm here\" ends the current storm; the next one forms at the chosen location.");
	}

	void SupercellWindow::RenderOverrides(const double *Player)
	{
		ImGui::TextDisabled("Makes the storm scripts read another position, without changing the location.");

		m_PositionMode = static_cast<int>(Supercells::GetPositionMode());

		bool changed = false;
		changed |= ImGui::RadioButton("Off", &m_PositionMode, 0);
		ImGui::SameLine();
		changed |= ImGui::RadioButton("Follow player", &m_PositionMode, 1);
		ImGui::SameLine();
		changed |= ImGui::RadioButton("Coordinates", &m_PositionMode, 2);

		if (m_PositionMode == 2)
		{
			changed |= ImGui::InputScalarN("XYZ", ImGuiDataType_Double, m_Position, 3, nullptr, nullptr, "%.0f");

			if (ImGui::Button("Use player position") && Player)
			{
				std::copy_n(Player, 3, m_Position);
				changed = true;
			}
		}

		changed |= ImGui::Checkbox("Keep storm altitude##override", &m_KeepAltitude);

		if (changed)
			Supercells::SetPositionOverride(static_cast<Supercells::PositionMode>(m_PositionMode), m_Position, m_KeepAltitude);

		ImGui::Separator();
		ImGui::TextDisabled("Writes the size facts of the current storm directly (0 = leave to the game).");
		bool sizeChanged = ImGui::Checkbox("Override size", &m_SizeOverride);

		if (m_SizeOverride)
		{
			sizeChanged |= ImGui::SliderFloat("Inner radius", &m_InnerRadius, 0.0f, 3000.0f, "%.0f");
			sizeChanged |= ImGui::SliderFloat("Outer radius", &m_OuterRadius, 0.0f, 4000.0f, "%.0f");
			sizeChanged |= ImGui::SliderFloat("Weather blend radius", &m_BlendRadius, 0.0f, 6000.0f, "%.0f");
		}

		if (sizeChanged)
			Supercells::SetSizeOverride(m_SizeOverride, m_InnerRadius, m_OuterRadius, m_BlendRadius);
	}

	bool SupercellWindow::Close()
	{
		return !m_WindowOpen;
	}

	std::string SupercellWindow::GetId() const
	{
		return "Supercell Storms";
	}
}
