#include <Windows.h>
#include <algorithm>
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
			{ "Rotation", GGUUID::Parse("BB7E8786-F350-ED44-B7FB-63E4715206A9") },
			{ "Variant", GGUUID::Parse("11B7A0B4-20C9-A444-83F8-DEFB65EA82F9") },
			{ "Storm up", GGUUID::Parse("FF6DACB9-2561-1545-82C4-4744E2B58B2B") },
			{ "Weather active", GGUUID::Parse("3970E323-EA24-2F41-BD95-8E72A5E56000") },
			{ "Intensity", GGUUID::Parse("3AE564BB-F7BC-A344-AAA5-D9A866EFF592") },
			{ "Lightning step", GGUUID::Parse("4E06708B-711A-BD43-BF8B-EC33FF8AE870") },
		};
	}

	void SupercellWindow::Render()
	{
		ImGui::SetNextWindowSize(ImVec2(620, 560), ImGuiCond_FirstUseEver);

		if (!ImGui::Begin(GetId().c_str(), &m_WindowOpen))
		{
			ImGui::End();
			return;
		}

		const auto status = Supercells::GetStatus();
		const auto now = GetTickCount64();

		if (!status.m_GraphHooks)
			ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Graph hooks are not installed yet (load a save).");
		else if (!status.m_ContainerHooked)
			ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Supercell container hook is missing, see mod_log.txt.");

		ImGui::Text("Container: %s (state %d)", Supercells::StateName(status.m_State), status.m_State);

		if (status.m_LastUpdateTick)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("updated %.1fs ago", (now - status.m_LastUpdateTick) / 1000.0);
		}

		if (status.m_TimerEnd > 0.0f)
		{
			ImGui::ProgressBar(
				status.m_TimerCurrent / status.m_TimerEnd,
				ImVec2(-FLT_MIN, 0),
				std::format("{:.0f} / {:.0f}", status.m_TimerCurrent, status.m_TimerEnd).c_str());
		}

		ImGui::TextDisabled("The game runs one supercell at a time; it moves to a new location every cycle.");
		ImGui::Separator();

		// Current storm parameters, as last read or written by the supercell scripts
		if (ImGui::BeginTable("##StormParams", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Parameter");
			ImGui::TableSetupColumn("Value");
			ImGui::TableSetupColumn("Source");
			ImGui::TableSetupColumn("Age");
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
					ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s *", fact->m_Value.c_str());
				else
					ImGui::TextUnformatted(fact->m_Value.c_str());

				ImGui::TableNextColumn();
				ImGui::Text("%s %s", fact->m_LastWasWrite ? "set by" : "read by", fact->m_Graph.substr(0, 24).c_str());

				ImGui::TableNextColumn();
				ImGui::Text("%.1fs", (now - fact->m_LastTick) / 1000.0);
			}

			ImGui::EndTable();
		}

		ImGui::Separator();

		// Cycle control
		if (ImGui::Button("Next storm now"))
			Supercells::RequestNextStorm();

		ImGui::SameLine();
		bool hold = Supercells::GetHoldStorm();

		if (ImGui::Checkbox("Hold current storm", &hold))
			Supercells::SetHoldStorm(hold);

		// Location
		ImGui::Separator();
		ImGui::TextUnformatted("Location");
		double player[3] = {};
		const bool havePlayer = Supercells::GetPlayerPosition(player);

		if (havePlayer)
			ImGui::TextDisabled("Player: %.0f, %.0f, %.0f", player[0], player[1], player[2]);

		bool changed = false;
		changed |= ImGui::RadioButton("Game decides", &m_PositionMode, 0);
		ImGui::SameLine();
		changed |= ImGui::RadioButton("Follow player", &m_PositionMode, 1);
		ImGui::SameLine();
		changed |= ImGui::RadioButton("Coordinates", &m_PositionMode, 2);

		if (m_PositionMode == 2)
		{
			changed |= ImGui::InputScalarN("XYZ", ImGuiDataType_Double, m_Position, 3, nullptr, nullptr, "%.0f");

			if (ImGui::Button("Use player position") && havePlayer)
			{
				std::copy_n(player, 3, m_Position);
				changed = true;
			}

			ImGui::SameLine();

			if (auto current = GraphRuntime::GetObservedFact(Params[0].m_Fact); ImGui::Button("Use storm position") && current)
			{
				std::copy_n(current->m_Position, 3, m_Position);
				changed = true;
			}
		}

		changed |= ImGui::Checkbox("Keep storm altitude (ignore Z)", &m_KeepAltitude);

		if (changed)
			Supercells::SetPositionOverride(static_cast<Supercells::PositionMode>(m_PositionMode), m_Position, m_KeepAltitude);

		if (ImGui::Button("Spawn storm above player") && havePlayer)
		{
			m_PositionMode = 2;
			std::copy_n(player, 3, m_Position);
			Supercells::SetPositionOverride(Supercells::PositionMode::Fixed, m_Position, m_KeepAltitude);
			Supercells::RequestNextStorm();
		}

		ImGui::SameLine();
		ImGui::TextDisabled("(ends the current storm; the next one forms over this point)");

		// Size
		ImGui::Separator();
		ImGui::TextUnformatted("Size");
		bool sizeChanged = ImGui::Checkbox("Override radii", &m_SizeOverride);

		if (m_SizeOverride)
		{
			sizeChanged |= ImGui::SliderFloat("Inner", &m_InnerRadius, 100.0f, 3000.0f, "%.0f");
			sizeChanged |= ImGui::SliderFloat("Outer", &m_OuterRadius, 100.0f, 4000.0f, "%.0f");
		}

		if (sizeChanged)
			Supercells::SetSizeOverride(m_SizeOverride, m_InnerRadius, m_OuterRadius);

		ImGui::End();
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
