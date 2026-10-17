#include <Windows.h>
#include <algorithm>
#include <format>
#include "GraphFactsWindow.h"

namespace HRZ2::DebugUI
{
	using namespace GraphRuntime;

	void GraphFactsWindow::Render()
	{
		ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

		if (!ImGui::Begin(GetId().c_str(), &m_WindowOpen))
		{
			ImGui::End();
			return;
		}

		if (!IsInstalled())
			ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "Graph hooks are not installed yet (watch a graph and load a save).");

		// Watched graphs
		if (ImGui::CollapsingHeader("Watched graphs", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::BeginTable("##Graphs", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("Tag");
				ImGui::TableSetupColumn("Trace to log", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Entry points", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Code (RVA)");
				ImGui::TableHeadersRow();

				for (const auto& g : GetWatchedGraphs())
				{
					ImGui::PushID(g.m_Tag.c_str());
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(g.m_Tag.c_str());

					ImGui::TableNextColumn();
					bool trace = g.m_Trace;

					if (ImGui::Checkbox("##trace", &trace))
						SetTrace(g.m_Tag, trace);

					ImGui::TableNextColumn();
					ImGui::Text("%zu", g.m_EntryPoints);

					ImGui::TableNextColumn();

					if (g.m_Begin)
						ImGui::Text("%llX-%llX", static_cast<unsigned long long>(g.m_Begin), static_cast<unsigned long long>(g.m_End));
					else
						ImGui::TextDisabled("not loaded");

					ImGui::PopID();
				}

				ImGui::EndTable();
			}

			ImGui::SetNextItemWidth(400);
			ImGui::InputTextWithHint("##NewTag", "part of an entry point name, e.g. SuperCellScriptContainer", m_NewTag, sizeof(m_NewTag));
			ImGui::SameLine();
			ImGui::Checkbox("Trace", &m_NewTagTrace);
			ImGui::SameLine();

			if (ImGui::Button("Watch") && m_NewTag[0])
			{
				WatchGraph(m_NewTag, m_NewTagTrace);
				m_NewTag[0] = '\0';
			}
		}

		// Overrides
		if (ImGui::CollapsingHeader("Overrides", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const auto overrides = GetOverrides();

			if (ImGui::BeginTable("##Overrides", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Fact");
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Graphs");
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableHeadersRow();

				for (size_t i = 0; i < overrides.size(); i++)
				{
					const auto& o = overrides[i];
					ImGui::PushID(static_cast<int>(i));
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					bool enabled = o.m_Enabled;

					if (ImGui::Checkbox("##on", &enabled))
						SetOverrideEnabled(i, enabled);

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(o.m_Source.c_str());

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(GetFactLabel(o.m_Fact).c_str());

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(KindName(o.m_Value.m_Kind));

					ImGui::TableNextColumn();
					std::string graphs;

					for (const auto& g : o.m_Graphs)
						graphs += (graphs.empty() ? "" : ", ") + g.substr(0, 28);

					ImGui::TextUnformatted(graphs.empty() ? "(all watched)" : graphs.c_str());

					ImGui::TableNextColumn();

					if (ImGui::SmallButton("Remove"))
						RemoveOverride(i);

					ImGui::PopID();
				}

				ImGui::EndTable();
			}

			if (ImGui::Button("Print overrides to log as config"))
			{
				for (const auto& o : overrides)
					spdlog::info("\n{}", OverrideToToml(o));
			}
		}

		// Observed facts
		if (ImGui::CollapsingHeader("Observed facts", ImGuiTreeNodeFlags_DefaultOpen))
		{
			m_Filter.Draw("Filter", 300);
			ImGui::SameLine();

			if (ImGui::Button("Clear"))
				ClearObservedFacts();

			auto facts = GetObservedFacts();
			std::ranges::sort(facts, [](const auto& A, const auto& B) { return A.m_LastTick > B.m_LastTick; });

			const auto now = GetTickCount64();
			constexpr auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

			if (ImGui::BeginTable("##Facts", 7, flags, ImVec2(0, 300)))
			{
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Graph");
				ImGui::TableSetupColumn("Fact");
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("Value");
				ImGui::TableSetupColumn("Count / age", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableHeadersRow();

				for (const auto& f : facts)
				{
					const auto label = GetFactLabel(f.m_Fact);

					if (!m_Filter.PassFilter(label.c_str()) && !m_Filter.PassFilter(f.m_Graph.c_str()) && !m_Filter.PassFilter(f.m_Value.c_str()))
						continue;

					ImGui::PushID(&f);
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(f.m_Graph.substr(0, 32).c_str());

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(label.c_str());

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(KindName(f.m_Kind));

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(f.m_LastWasWrite ? "write" : "read");

					ImGui::TableNextColumn();

					if (f.m_Overridden)
						ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "%s *", f.m_Value.c_str());
					else
						ImGui::TextUnformatted(f.m_Value.c_str());

					ImGui::TableNextColumn();
					ImGui::Text("%llu / %.1fs", static_cast<unsigned long long>(f.m_Count), (now - f.m_LastTick) / 1000.0);

					ImGui::TableNextColumn();

					if (ImGui::SmallButton("Override"))
					{
						m_EditFact = f;
						const auto text = f.m_Kind == FactKind::Enum ? std::string("default") : f.m_Value.substr(0, f.m_Value.find(' '));
						strncpy_s(m_EditValue, text.c_str(), _TRUNCATE);
						ImGui::OpenPopup("##EditOverride");
					}

					if (ImGui::BeginPopup("##EditOverride"))
					{
						ImGui::Text("%s (%s)", GetFactLabel(m_EditFact.m_Fact).c_str(), KindName(m_EditFact.m_Kind));
						ImGui::TextDisabled("Bool: true/false, Enum: default or entry UUID, Position: x,y,z (* keeps an axis)");
						ImGui::InputText("Value", m_EditValue, sizeof(m_EditValue));
						ImGui::Checkbox("Apply to all watched graphs", &m_EditAllGraphs);

						const auto parsed = ParseOverrideValue(m_EditFact.m_Kind, m_EditValue);

						if (!parsed)
							ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Value does not match the fact type");

						if (ImGui::Button("Add override") && parsed)
						{
							FactOverride o;
							o.m_Fact = m_EditFact.m_Fact;
							o.m_Value = *parsed;
							o.m_Source = "ui";

							if (!m_EditAllGraphs)
								o.m_Graphs = { m_EditFact.m_Graph };

							AddOverride(std::move(o));
							ImGui::CloseCurrentPopup();
						}

						ImGui::EndPopup();
					}

					ImGui::PopID();
				}

				ImGui::EndTable();
			}
		}

		ImGui::End();
	}

	bool GraphFactsWindow::Close()
	{
		return !m_WindowOpen;
	}

	std::string GraphFactsWindow::GetId() const
	{
		return "Graph Facts";
	}
}
