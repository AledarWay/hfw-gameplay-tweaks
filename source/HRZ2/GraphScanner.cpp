#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <shared_mutex>
#include <string>
#include <vector>
#include "../ModConfiguration.h"
#include "../ModCoreEvents.h"
#include "Core/ProgramResource.h"
#include "Core/RTTI.h"
#include "Core/RTTIRefObject.h"
#include "GraphScanner.h"

namespace HRZ2::GraphScanner
{
	namespace
	{
		std::string Lower(std::string_view Text)
		{
			std::string result(Text);
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		std::vector<std::string> ParseKeywords()
		{
			std::string source = ModConfiguration.GraphScanKeywords;

			if (source.empty())
				source = "supercell,storm,aether,weather";

			std::vector<std::string> keywords;

			std::string word;

			for (const char c : source + ",")
			{
				if (c != ',')
				{
					if (!std::isspace(static_cast<unsigned char>(c)))
						word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

					continue;
				}

				if (!word.empty())
					keywords.emplace_back(std::move(word));

				word.clear();
			}

			return keywords;
		}

		bool Matches(std::string_view Text, const std::vector<std::string>& Keywords)
		{
			const auto lowered = Lower(Text);
			return std::ranges::any_of(Keywords, [&](const auto& k) { return lowered.find(k) != std::string::npos; });
		}

		bool ObjectsMatch(const Array<Ref<RTTIRefObject>>& Objects, const std::vector<std::string>& Keywords)
		{
			for (size_t i = 0; i < Objects.size(); i++)
			{
				if (auto obj = Objects[i].GetPtr(); obj && Matches(obj->GetRTTI()->GetSymbolName(), Keywords))
					return true;
			}

			return false;
		}

		bool ListMatches(const ProgramParameterList& List, const std::vector<std::string>& Keywords)
		{
			for (size_t i = 0; i < List.m_Parameters.size(); i++)
			{
				const auto& param = List.m_Parameters[i];

				if (Matches(param.m_Name.c_str(), Keywords) || Matches(param.m_Type.m_StaticTypeName.c_str(), Keywords))
					return true;
			}

			return ObjectsMatch(List.m_DefaultSoftLinkedObjects, Keywords) || ObjectsMatch(List.m_DefaultHardLinkedObjects, Keywords);
		}

		void WriteObjects(std::string& Out, const char *Kind, const Array<Ref<RTTIRefObject>>& Objects)
		{
			for (size_t i = 0; i < Objects.size(); i++)
			{
				const auto obj = Objects[i].GetPtr();

				if (!obj)
				{
					Out += std::format("    {}{}: null\n", Kind, i);
					continue;
				}

				const auto rtti = obj->GetRTTI();
				const auto typeName = rtti->GetSymbolName();
				Out += std::format("    {}{}: {} {}\n", Kind, i, typeName, obj->m_UUID);

				// Facts and conditions are what gates the graph, so show their contents
				if (Matches(typeName, { "fact", "condition", "weathersetup" }))
					Out += std::format("{}\n", rtti->AsCompound()->SerializeObject(obj, 2).value_or("<FAILED>"));
			}
		}

		void WriteList(std::string& Out, std::string_view Title, const ProgramParameterList& List)
		{
			Out += std::format(
				"  [{}] params={} soft={} hard={} uuidrefs={}\n",
				Title,
				List.m_Parameters.size(),
				List.m_DefaultSoftLinkedObjects.size(),
				List.m_DefaultHardLinkedObjects.size(),
				List.m_DefaultUUIDRefs.size());

			for (size_t i = 0; i < List.m_Parameters.size(); i++)
			{
				const auto& param = List.m_Parameters[i];
				Out += std::format(
					"    p{}: '{}' : {} flags={:#x} hash={:#x}\n",
					i,
					param.m_Name.c_str(),
					param.m_Type.m_StaticTypeName.c_str(),
					param.m_FlagsAndIndex,
					param.m_UUIDHash);
			}

			WriteObjects(Out, "soft", List.m_DefaultSoftLinkedObjects);
			WriteObjects(Out, "hard", List.m_DefaultHardLinkedObjects);

			for (size_t i = 0; i < List.m_DefaultUUIDRefs.size(); i++)
				Out += std::format("    uuid{}: {}\n", i, List.m_DefaultUUIDRefs[i]);
		}
	}

	size_t ScanCachedGraphPrograms()
	{
		const auto keywords = ParseKeywords();
		auto& events = ModCoreEvents::GetInstance();

		std::vector<Ref<RTTIRefObject>> graphs;
		{
			std::shared_lock lock(events.m_CachedDataMutex);
			graphs.assign(events.m_CachedGraphPrograms.begin(), events.m_CachedGraphPrograms.end());
		}

		std::string out;
		size_t matches = 0;

		for (auto& graph : graphs)
		{
			try
			{
				const auto program = static_cast<ProgramResource *>(graph.GetPtr());
				const auto& state = graph->GetMemberRefUnsafe<ProgramParameterList>("StateParameters");
				const auto exposed = graph->GetMemberRefUnsafe<Ref<RTTIRefObject>>("ExposedDataResource").GetPtr();
				const ProgramParameterList *exposedParams = nullptr;

				if (exposed)
				{
					try
					{
						exposedParams = &exposed->GetMemberRefUnsafe<ProgramParameterList>("Parameters");
					}
					catch (const std::exception&)
					{
					}
				}

				bool match = ListMatches(state, keywords) || (exposedParams && ListMatches(*exposedParams, keywords));

				for (size_t i = 0; !match && i < program->m_EntryPoints.size(); i++)
				{
					const auto& entry = program->m_EntryPoints[i];
					match = Matches(entry.m_EntryPoint.c_str(), keywords) || ListMatches(entry.m_InputParameters, keywords) ||
							ListMatches(entry.m_OutputParameters, keywords);
				}

				if (!match)
					continue;

				matches++;
				out += std::format("==== GraphProgramResource {}\n", graph->m_UUID);
				WriteList(out, "State", state);

				if (exposedParams)
					WriteList(out, std::format("Exposed {} {}", exposed->GetRTTI()->GetSymbolName(), exposed->m_UUID), *exposedParams);

				for (size_t i = 0; i < program->m_EntryPoints.size(); i++)
				{
					const auto& entry = program->m_EntryPoints[i];
					WriteList(out, std::format("Entry '{}' in", entry.m_EntryPoint.c_str()), entry.m_InputParameters);
					WriteList(out, std::format("Entry '{}' out", entry.m_EntryPoint.c_str()), entry.m_OutputParameters);
				}

				out += "\n";
			}
			catch (const std::exception& e)
			{
				out += std::format("==== GraphProgramResource {} <error: {}>\n\n", graph->m_UUID, e.what());
			}
		}

		FILE *f = nullptr;

		if (_wfopen_s(&f, InternalModConfig::GetModRelativePath("graph_scan.txt").c_str(), L"wb") == 0 && f)
		{
			fwrite(out.data(), 1, out.size(), f);
			fclose(f);
		}

		spdlog::info("Graph scan: {} of {} cached graph programs matched", matches, graphs.size());
		return matches;
	}
}
