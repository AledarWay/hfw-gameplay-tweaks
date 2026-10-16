#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <shared_mutex>
#include <string>
#include <vector>
#include "../ModConfiguration.h"
#include "../ModCoreEvents.h"
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

		FILE *f = nullptr;

		if (_wfopen_s(&f, InternalModConfig::GetModRelativePath("graph_scan.txt").c_str(), L"wb") != 0 || !f)
			return 0;

		spdlog::info("Graph scan: {} cached graph programs, keywords '{}'", graphs.size(), ModConfiguration.GraphScanKeywords);

		// The game's own serializer knows the real layouts, so nothing here reads raw structs
		const auto depth = static_cast<size_t>(std::clamp(std::max(ModConfiguration.AssetDumpDepth, 5), 1, 8));
		size_t matches = 0;

		for (size_t i = 0; i < graphs.size(); i++)
		{
			const auto graph = graphs[i].GetPtr();

			if (i % 250 == 0)
				spdlog::info("Graph scan: {}/{} next {}", i, graphs.size(), graph->m_UUID);

			const auto text = graph->GetRTTI()->AsCompound()->SerializeObject(graph, depth);

			if (!text)
				continue;

			const auto lowered = Lower(*text);

			std::vector<std::string_view> hits;
			for (const auto& k : keywords)
			{
				if (lowered.find(k) != std::string::npos)
					hits.emplace_back(k);
			}

			if (hits.empty())
				continue;

			matches++;

			std::string header = std::format("==== GraphProgramResource {} hits:", graph->m_UUID);
			for (auto h : hits)
				header += std::format(" {}", h);
			header += "\n";

			fwrite(header.data(), 1, header.size(), f);
			fwrite(text->data(), 1, text->size(), f);
			fwrite("\n\n", 1, 2, f);
			fflush(f);
		}

		fclose(f);

		spdlog::info("Graph scan: {} of {} cached graph programs matched", matches, graphs.size());
		return matches;
	}
}
