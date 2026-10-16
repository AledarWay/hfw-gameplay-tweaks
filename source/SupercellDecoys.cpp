#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "HRZ2/Core/ProgramResource.h"
#include "HRZ2/Core/RTTI.h"
#include "HRZ2/Core/RTTIRefObject.h"
#include "ModConfiguration.h"
#include "SupercellDecoys.h"

namespace SupercellDecoys
{
	using namespace HRZ2;

	namespace
	{
		constexpr auto QuestStateDefinitionUUID = GGUUID::Parse("F672389F-F41B-D84F-8225-55DB636498CD");

		std::mutex g_Mutex;
		RTTIRefObject *g_Holder = nullptr;
		bool g_Pending = false;

		struct Rule
		{
			std::optional<GGUUID> m_Fact;
			bool m_AllQuestStates = false;
			int m_SlotFirst = -1;
			int m_SlotLast = -1;
			std::string m_Value;
		};

		std::string Trim(std::string_view Text)
		{
			const auto first = Text.find_first_not_of(" \t");
			const auto last = Text.find_last_not_of(" \t");
			return first == std::string_view::npos ? std::string() : std::string(Text.substr(first, last - first + 1));
		}

		// Format: "KEY=VALUE, ..." where KEY is a fact UUID, "quests", "#N" or "#N-M" (soft link slots)
		std::vector<Rule> ParseRules()
		{
			std::vector<Rule> rules;
			const std::string source = ModConfiguration.SupercellFactDecoys + ",";
			size_t start = 0;

			for (size_t comma; (comma = source.find(',', start)) != std::string::npos; start = comma + 1)
			{
				const auto entry = Trim(std::string_view(source).substr(start, comma - start));
				const auto eq = entry.find('=');

				if (entry.empty() || eq == std::string::npos)
					continue;

				Rule rule;
				const auto key = Trim(std::string_view(entry).substr(0, eq));
				rule.m_Value = Trim(std::string_view(entry).substr(eq + 1));

				if (key == "quests")
				{
					rule.m_AllQuestStates = true;
				}
				else if (key.starts_with('#'))
				{
					const auto dash = key.find('-');

					try
					{
						rule.m_SlotFirst = std::stoi(key.substr(1, dash == std::string::npos ? std::string::npos : dash - 1));
						rule.m_SlotLast = dash == std::string::npos ? rule.m_SlotFirst : std::stoi(key.substr(dash + 1));
					}
					catch (const std::exception&)
					{
						spdlog::warn("SupercellFactDecoys: bad slot '{}'", key);
						continue;
					}
				}
				else if (auto uuid = GGUUID::TryParse(key))
				{
					rule.m_Fact = uuid;
				}
				else
				{
					spdlog::warn("SupercellFactDecoys: bad key '{}'", key);
					continue;
				}

				rules.emplace_back(std::move(rule));
			}

			return rules;
		}

		const Rule *FindRule(const std::vector<Rule>& Rules, RTTIRefObject *Fact, int Slot, bool IsQuestState)
		{
			for (const auto& rule : Rules)
			{
				if ((rule.m_Fact && *rule.m_Fact == Fact->m_UUID) || (rule.m_AllQuestStates && IsQuestState) ||
					(Slot >= rule.m_SlotFirst && Slot <= rule.m_SlotLast))
					return &rule;
			}

			return nullptr;
		}

		std::optional<GGUUID> ResolveEnumValue(const std::string& Value, const GGUUID& Original)
		{
			if (_stricmp(Value.c_str(), "InProgress") == 0)
				return GGUUID::Parse("AE6A579F-D1DD-8441-856F-5599DB49825D");

			if (_stricmp(Value.c_str(), "Succeeded") == 0)
				return GGUUID::Parse("EF0BC82F-5960-DB4B-9251-7F7D468AFE65");

			if (_stricmp(Value.c_str(), "Default") == 0)
				return Original;

			return GGUUID::TryParse(Value);
		}

		Ref<RTTIRefObject> MakeDecoy(RTTIRefObject *Original, const std::string& Value)
		{
			const auto rtti = Original->GetRTTI();
			const auto typeName = rtti->GetSymbolName();

			if (typeName != "BooleanFact" && typeName != "EnumFact")
				return {};

			Ref<RTTIRefObject> decoy(static_cast<RTTIRefObject *>(rtti->CreateInstance()));

			if (!decoy.GetPtr())
				return {};

			// Fresh UUID so the fact database never has a saved value for it
			auto uuidText = std::format("{}", Original->m_UUID);
			uuidText.replace(0, 4, "DEC0");
			decoy->m_UUID = GGUUID::Parse(uuidText);

			decoy->GetMemberRefUnsafe<bool>("Persistent") = false;
			decoy->GetMemberRefUnsafe<bool>("Replicated") = Original->GetMemberRefUnsafe<bool>("Replicated");
			decoy->GetMemberRefUnsafe<bool>("CanSetByScript") = false;

			if (typeName == "BooleanFact")
			{
				decoy->GetMemberRefUnsafe<bool>("DefaultValue") = _stricmp(Value.c_str(), "true") == 0 || Value == "1";
			}
			else
			{
				const auto value = ResolveEnumValue(Value, Original->GetMemberRefUnsafe<GGUUID>("DefaultValueUUID"));

				if (!value)
					return {};

				decoy->GetMemberRefUnsafe<Ref<RTTIRefObject>>("EnumDefinition") =
					Original->GetMemberRefUnsafe<Ref<RTTIRefObject>>("EnumDefinition");
				decoy->GetMemberRefUnsafe<GGUUID>("DefaultValueUUID") = *value;
			}

			return decoy;
		}

		bool IsQuestStateFact(RTTIRefObject *Fact)
		{
			if (Fact->GetRTTI()->GetSymbolName() != "EnumFact")
				return false;

			const auto definition = Fact->GetMemberRefUnsafe<Ref<RTTIRefObject>>("EnumDefinition").GetPtr();
			return definition && definition->m_UUID == QuestStateDefinitionUUID;
		}

		void Apply(RTTIRefObject *Holder)
		{
			const auto rules = ParseRules();
			auto& list = Holder->GetMemberRefUnsafe<ProgramParameterList>("Parameters").m_DefaultSoftLinkedObjects;

			spdlog::info("Supercell decoys: holder {} has {} soft links, {} rules", Holder->m_UUID, list.size(), rules.size());

			for (size_t i = 0; i < list.size(); i++)
			{
				const auto fact = list[i].GetPtr();

				if (!fact)
					continue;

				const auto typeName = fact->GetRTTI()->GetSymbolName();

				if (!typeName.ends_with("Fact"))
					continue;

				const bool questState = IsQuestStateFact(fact);
				const auto rule = FindRule(rules, fact, static_cast<int>(i), questState);
				const char *status = "";

				if (rule && !std::format("{}", fact->m_UUID).starts_with("DEC0"))
				{
					if (auto decoy = MakeDecoy(fact, rule->m_Value); decoy.GetPtr())
					{
						list[i] = decoy;
						status = " -> DECOY";
					}
					else
					{
						status = " -> decoy failed";
					}
				}

				spdlog::info(
					"  #{} {} {}{}{}{}",
					i,
					typeName,
					fact->m_UUID,
					questState ? " [quest state]" : "",
					rule ? std::format(" rule '{}'", rule->m_Value) : "",
					status);
			}
		}
	}

	void OnHolderLoaded(RTTIRefObject *Holder)
	{
		std::scoped_lock lock(g_Mutex);

		g_Holder = Holder;
		g_Pending = true;
	}

	void OnHolderUnloaded(RTTIRefObject *Holder)
	{
		std::scoped_lock lock(g_Mutex);

		if (g_Holder == Holder)
		{
			g_Holder = nullptr;
			g_Pending = false;
		}
	}

	// Called after a load group finishes, when cross-file references should be resolved
	void ApplyPending()
	{
		std::scoped_lock lock(g_Mutex);

		if (!g_Pending || !g_Holder)
			return;

		g_Pending = false;
		Apply(g_Holder);
	}

	void ApplyNow()
	{
		std::scoped_lock lock(g_Mutex);

		if (!g_Holder)
		{
			spdlog::warn("Supercell decoys: SuperCellScriptContainer constants are not loaded");
			return;
		}

		g_Pending = false;
		Apply(g_Holder);
	}
}
