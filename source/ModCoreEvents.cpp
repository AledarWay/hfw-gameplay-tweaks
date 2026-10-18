#include <Windows.h>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <cstdio>
#include <format>
#include <ranges>
#include <shared_mutex>
#include "HRZ2/Core/EntityComponent.h"
#include "HRZ2/Core/IStreamingManager.h"
#include "HRZ2/Core/JobHeaderCPU.h"
#include "HRZ2/Core/ProgramResource.h"
#include "HRZ2/Core/RTTI.h"
#include "HRZ2/Core/RTTIRefObject.h"
#include "ModConfiguration.h"
#include "ModCoreEvents.h"
#include "HRZ2/RTTIScanner.h"
#include "HRZ2/GraphRuntime.h"
#include "Htn.h"
#include "Supercells.h"

ModCoreEvents::ValuePatchVisitor::ValuePatchVisitor(const RTTIValuePatch& Patch)
{
	m_ValueToSet = Patch.m_Value;
}

void ModCoreEvents::ValuePatchVisitor::Visit(void *Object, const HRZ2::RTTI *Type)
{
	if (!Type->DeserializeObject(Object, m_ValueToSet))
		m_LastError = std::format("Failed to set value '{}'.", m_ValueToSet);
}

HRZ2::RTTIObjectTweaker::SetValueVisitor::EMode ModCoreEvents::ValuePatchVisitor::GetMode()
{
	return EMode::MODE_WRITE;
}

ModCoreEvents::ModCoreEvents()
{
	for (const auto& c : ModConfiguration.ComponentInjects)
	{
		if (!c.Enabled)
			continue;

		auto parse = [](const std::string& Text) { return Text.empty() ? std::nullopt : HRZ2::GGUUID::TryParse(Text); };
		auto target = parse(c.Target);
		auto contains = parse(c.TargetContains);
		auto component = parse(c.Component);

		if (component && !c.Field.empty() && target)
			m_ComponentInjects.emplace_back(ComponentInject { *target, c.Field, *component });
		else if (component && !c.Field.empty() && !c.TargetType.empty() && contains)
			m_ComponentInjectRules.emplace_back(ComponentInjectRule { c.TargetType, *contains, c.Field, *component });
		else
			spdlog::error("Invalid [[ComponentInject]] entry: Target '{}', TargetType '{}', TargetContains '{}', Field '{}', Component '{}'",
				c.Target, c.TargetType, c.TargetContains, c.Field, c.Component);
	}

	for (const auto& part : ModConfiguration.AssetFindReferences | std::views::split(','))
	{
		std::string text { std::string_view(part) };
		std::erase_if(text, [](unsigned char c) { return std::isspace(c); });

		if (auto uuid = HRZ2::GGUUID::TryParse(text))
			m_RefFindUUIDs.emplace(*uuid);
		else if (!text.empty())
			spdlog::error("Invalid AssetFindReferences UUID '{}'", text);
	}

	if (!m_ComponentInjects.empty() || !m_ComponentInjectRules.empty())
		spdlog::info("[inject] {} fixed targets, {} type rules", m_ComponentInjects.size(), m_ComponentInjectRules.size());

	// Comma-separated, case-insensitive substrings of RTTI type names or object UUIDs
	for (const auto& part : ModConfiguration.AssetDumpTypes | std::views::split(','))
	{
		std::string filter { std::string_view(part) };
		std::erase_if(filter, [](unsigned char c) { return std::isspace(c); });
		std::ranges::transform(filter, filter.begin(), [](unsigned char c) { return std::tolower(c); });

		if (!filter.empty())
			m_AssetDumpFilters.emplace_back(std::move(filter));
	}

	RegisterParsedPatches();
	RegisterHardcodedPatches();
}

void ModCoreEvents::RegisterParsedPatches()
{
	auto splitStringByDelimiter = [](const std::string_view& Text, char Delimiter, auto&& Callback)
	{
		for (const auto& part : Text | std::views::split(Delimiter))
			Callback(std::string_view(part));
	};

	// Create all of the patch instances from mod configuration data
	for (const auto& entry : ModConfiguration.AssetOverrides)
	{
		if (!entry.Enabled)
			continue;

		if (entry.ObjectTypes.empty() && entry.ObjectUUIDs.empty())
			spdlog::error("ObjectTypes and ObjectUUIDs are both empty. Asset override will have no effect.");

		// Lookup by type
		splitStringByDelimiter(
			entry.ObjectTypes,
			',',
			[&](const std::string_view& Type)
			{
				auto rtti = HRZ2::RTTI::FindTypeByName(Type);

				if (rtti)
					m_RTTIPatchesByType[rtti].emplace_back(RTTIValuePatch { entry.Path, entry.Value });
				else
					spdlog::error("Failed to resolve override type name '{}'. Skipping.", Type);
			});

		// Lookup by UUID
		splitStringByDelimiter(
			entry.ObjectUUIDs,
			',',
			[&](const std::string_view& UUID)
			{
				auto uuid = HRZ2::GGUUID::TryParse(UUID);

				if (uuid)
					m_RTTIPatchesByUUID[uuid.value()].emplace_back(RTTIValuePatch { entry.Path, entry.Value });
				else
					spdlog::error("Failed to parse override UUID '{}'. Skipping.", UUID);
			});
	}

	// Then also handle character override entries
	for (const auto& entry : ModConfiguration.CharacterOverrides)
	{
		if (entry.RootUUID.empty() || entry.VariantUUID.empty())
		{
			spdlog::error("RootUUID or VariantUUID is empty. Character override will have no effect.");
			continue;
		}

		m_CharacterOverrideRootUUID = entry.RootUUID;
		m_CharacterOverrideVariantUUID = entry.VariantUUID;
	}

	if (m_CharacterOverrideVariantUUID)
		spdlog::info("Using character override variant UUID {}", m_CharacterOverrideVariantUUID.value());
}

void ModCoreEvents::RegisterHardcodedPatches()
{
	using namespace HRZ2;

	auto addCallback = [this]<typename T>(T Value, void (*Callback)(Ref<RTTIRefObject>&))
	{
		if constexpr (std::is_same_v<T, GGUUID>)
			m_RTTIPatchesByUUID[Value].emplace_back(RTTICallbackPatch { Callback });
		else if constexpr (std::is_same_v<T, const RTTI *>)
			m_RTTIPatchesByType[Value].emplace_back(RTTICallbackPatch { Callback });
		else
			static_assert(!sizeof(T), "Invalid type");
	};

	auto addUnloadCallback = [this](const RTTI *Value, void (*Callback)(Ref<RTTIRefObject>&))
	{
		m_RTTIUnloadsByType[Value].emplace_back(Callback);
	};

	//
	// HFW hardcodes ultra hard difficulty in various places. Two common cases are direct pointer comparisons in HFW.exe
	// code and in fullgame graph programs. Menus use some kind of logic I haven't identified yet.
	//
	// UH's DifficultyResource gets loaded first at startup and stays in memory forever. The OnDifficultySetGraphProgram's
	// NodeConstantsResource and AIManager's AIManagerResourceGame can then reference it any time later.
	//
	if (ModConfiguration.ForceCustomUltraHard)
	{
		static Ref<RTTIRefObject> customDifficultyRef;

		addCallback(
			GGUUID::Parse("8B0BFB86-3540-1040-B856-488DA2D9E906"), // DifficultyResource
			[](auto& Object)
			{
				customDifficultyRef = Object;
			});

		addCallback(
			GGUUID::Parse("8B27A783-AB20-1A3B-8C6B-30330C9211BB"), // NodeConstantsResource
			[](auto& Object)
			{
				auto& parameters = Object->GetMemberRefUnsafe<ProgramParameterList>("Parameters");
				parameters.m_DefaultSoftLinkedObjects[0] = customDifficultyRef;
			});

		addCallback(
			GGUUID::Parse("8070E517-98AE-F347-BB49-3E87F3248E11"), // AIManagerResourceGame
			[](auto& Object)
			{
				Object->GetMemberRefUnsafe<Ref<RTTIRefObject>>("UltraHardDifficultyPreset") = customDifficultyRef;
			});
	}

	//
	// Remove the BoolFactConditionResource "is UH" check from HUDEnemyHealthBarLogicResource's DisplayConditions. Asset
	// overrides don't support arrays of references. We have to do it here instead.
	//
	if (ModConfiguration.UnlockUltraHardRestrictions)
	{
		addCallback(
			GGUUID::Parse("E1258F7C-DA72-5D48-8F62-3257B02C6BA6"), // HUDEnemyHealthBarLogicResource
			[](auto& Object)
			{
				auto& displayConditions = Object->GetMemberRefUnsafe<Array<Ref<RTTIRefObject>>>("DisplayConditions");

				if (displayConditions.size() == 5)
					displayConditions.pop_back();
			});
	}

	//
	// Increase inventory item stacks to 1,000,000 or multiply defaults by a user-defined value.
	//
	if (ModConfiguration.IncreaseInventoryStacks || ModConfiguration.InventoryStackMultiplier > 0)
	{
		addCallback(
			RTTI::FindTypeByName("InventoryItemResource"),
			[](auto& Object)
			{
				if (Object->GetMemberRefUnsafe<bool>("IsStackable"))
				{
					auto& stackSizes = Object->GetMemberRefUnsafe<Array<int>>("MaxStackSize");

					for (auto& limit : stackSizes)
					{
						if (ModConfiguration.InventoryStackMultiplier > 0)
							limit *= ModConfiguration.InventoryStackMultiplier;
						else
							limit = std::max(limit, 1000000);
					}
				}
			});

		addCallback(
			RTTI::FindTypeByName("InventoryCapacityComponentResource"),
			[](auto& Object)
			{
				class SharedInventoryItemTagLimitDefinition
				{
				public:
					Ref<RTTIRefObject> m_InventoryItemTag;
					Array<int> m_Limits;
					Ref<RTTIRefObject> m_LimitIndexFact;
				};
				static_assert(sizeof(SharedInventoryItemTagLimitDefinition) == 0x20);

				auto& limitDefinitions = Object->GetMemberRefUnsafe<Array<SharedInventoryItemTagLimitDefinition>>(
					"SharedInventoryItemLimits");

				for (auto& limitDef : limitDefinitions)
				{
					for (auto& limit : limitDef.m_Limits)
					{
						const auto hardCap = std::max(limit, 1000); // Higher than 1000 leads to UI-related crashes

						if (limit > 0)
						{
							if (ModConfiguration.InventoryStackMultiplier > 0)
								limit = std::min(limit * ModConfiguration.InventoryStackMultiplier, hardCap);
							else
								limit = hardCap;
						}
					}
				}
			});
	}

	//
	// Remove magnet-based aim assist and camera anchoring. Most obvious use is when targeting machines with the focus.
	//
	if (ModConfiguration.DisableCameraMagnetism || ModConfiguration.DisableFocusMagnetism)
	{
		auto killMagnetSelector = [](auto& Object)
		{
			class CameraParmResource
			{
			public:
				virtual ~CameraParmResource() = 0;

				bool m_Override;
				Ref<RTTIRefObject> m_Val;
			};
			static_assert(sizeof(CameraParmResource) == 0x18);

			auto& selector = Object->GetMemberRefUnsafe<CameraParmResource>("MagnetModeSelector");
			selector.m_Override = true;
			selector.m_Val = nullptr;
		};

		// CameraModeResource: everything
		if (ModConfiguration.DisableCameraMagnetism)
			addCallback(RTTI::FindTypeByName("CameraModeResource"), killMagnetSelector);

		if (ModConfiguration.DisableFocusMagnetism)
		{
			addCallback(GGUUID::Parse("A7493033-3D69-C94E-9791-641089B1FE46"), killMagnetSelector); // On foot, focus open
			addCallback(GGUUID::Parse("3AF1EE58-0F6C-A74E-A2FB-833376A2CAE7"), killMagnetSelector); // On foot, crouched, focus open
			addCallback(GGUUID::Parse("12C9ED87-A030-1D42-88C7-4ED6CB4356FC"), killMagnetSelector); // Swimming, focus open
			addCallback(GGUUID::Parse("FFDCCC96-898F-2B4A-846C-71F1B211BD23"), killMagnetSelector); // Swimming, underwater ruins, focus open
			addCallback(GGUUID::Parse("ED1E28E1-9CFE-2B4F-AEB1-7E26D976EC91"), killMagnetSelector); // Charger mount, focus open
			addCallback(GGUUID::Parse("CFB94F63-FD50-F54C-8AAF-21FEB059AB4D"), killMagnetSelector); // Clawstrider mount, focus open
			addCallback(GGUUID::Parse("D7B8892F-8ED5-0449-B540-316151575AEF"), killMagnetSelector); // Flying mount, focus open
		}
	}

	//
	// Character overrides. Direct BodyVariant pointer replacement.
	//
	if (m_CharacterOverrideVariantUUID)
	{
		static StreamingRef<RTTIRefObject> targetRootObject; // Permanently loaded
		static Ref<RTTIRefObject> targetBodyVariantObject;

		addCallback(
			GGUUID::Parse("1C710F84-C54D-9643-8353-379EC8927545"),
			[](auto& Object)
			{
				// Queue the load after the inital photo mode resource. Otherwise we'd need a hook.
				JobHeaderCPU::SubmitCallback(
					[]()
					{
						auto manager = StreamingManager::GetInstance();
						manager->Register2(targetRootObject, {}, GetInstance().m_CharacterOverrideRootUUID.value());
						manager->Resolve(targetRootObject, EStreamingRefPriority::Normal);
					});
			});

		addCallback(
			m_CharacterOverrideVariantUUID.value(),
			[](auto& Object)
			{
				std::scoped_lock lock(GetInstance().m_CachedDataMutex);
				targetBodyVariantObject = Object;
			});

		addCallback(
			RTTI::FindTypeByName("PlayerOutfitTheme"),
			[](auto& Object)
			{
				auto& variantRef = Object->GetMemberRefUnsafe<StreamingRef<RTTIRefObject>>("BodyVariant");
				variantRef.Clear();

				std::shared_lock lock(GetInstance().m_CachedDataMutex);
				StreamingManager::GetInstance()->Register4(variantRef, targetBodyVariantObject, 5);
			});
	}

	//
	// Custom damage multiplier overrides.
	//
	if (!ModConfiguration.DamageModifierOverrides.empty())
	{
		using DamageTypeResource = RTTIRefObject;
		using DamageTypeGroup = RTTIRefObject;
		using PlayerDamageModifier = RTTIRefObject;

		static Ref<RTTIRefObject> damageTypeResourceSettings;

		addCallback(
			RTTI::FindTypeByName("DamageTypeResourceSettingsGame"),
			[](auto& Object)
		{
			std::scoped_lock lock(GetInstance().m_CachedDataMutex);
			damageTypeResourceSettings = Object;
		});

		static auto getDamageTypeResourceByUUID = [](const GGUUID& UUID) -> Ref<DamageTypeResource>
		{
			std::shared_lock lock(GetInstance().m_CachedDataMutex);
			const auto& damageTypes = damageTypeResourceSettings->GetMemberRefUnsafe<Array<Ref<DamageTypeResource>>>("DamageTypeResources");

			for (const auto& type : damageTypes)
			{
				if (type->m_UUID == UUID)
					return type;
			}

			return nullptr;
		};

		addCallback(
			RTTI::FindTypeByName("DifficultyManagerResource"),
			[](auto& Object)
		{
			if (auto& globalDamageModifier = Object->GetMemberRefUnsafe<Ref<RTTIRefObject>>("GlobalDamageModifier"))
			{
				class PlayerDamageModifierForDamageType
				{
				public:
					Array<Ref<DamageTypeResource>> m_DamageTypes; // 0x0
					Ref<DamageTypeGroup> m_DamageTypeGroup;		  // 0x10
					alignas(16) float m_UnknownFloats[4] = {};	  // 0x20
					bool m_UnknownFlag = false;					  // 0x30
					char _pad31[0xF];							  // 0x31
					Ref<PlayerDamageModifier> m_DamageModifier;	  // 0x40
				};
				static_assert(sizeof(PlayerDamageModifierForDamageType) == 0x50);

				auto& specificDamageSettings = globalDamageModifier->GetMemberRefUnsafe<Array<PlayerDamageModifierForDamageType>>(
					"SpecificPlayerDamageDealtSettings");

				// We have to create a copy since old modifier values act as a fallback (they're evaluated last)
				const auto oldSpecificDamageSettings = std::move(specificDamageSettings);

				// Then convert mod-side overrides to the game's format
				for (const auto& modOverride : ModConfiguration.DamageModifierOverrides)
				{
					auto damageTypeResource = getDamageTypeResourceByUUID(modOverride.DamageTypeUUID);

					if (!damageTypeResource)
					{
						spdlog::error("Failed to find damage type resource with UUID '{}'. Skipping.", modOverride.DamageTypeUUID);
						continue;
					}

					const auto rtti = RTTI::FindTypeByName("PlayerDamageModifier")->AsCompound();
					Ref rttiInstance = static_cast<PlayerDamageModifier *>(rtti->CreateInstance());

					rtti->SetMemberValue<GGUUID>(rttiInstance, "ObjectUUID", GGUUID::Generate());
					rtti->SetMemberValue<float>(rttiInstance, "DamageModifier", modOverride.Multiplier);

					PlayerDamageModifierForDamageType newDamageSetting;
					newDamageSetting.m_DamageTypes.push_back(damageTypeResource);
					newDamageSetting.m_DamageModifier = rttiInstance;
					specificDamageSettings.push_back(newDamageSetting);
				}

				for (const auto& oldSetting : oldSpecificDamageSettings)
					specificDamageSettings.push_back(oldSetting);
			}
		});
	}

	//
	// Dialogue blacklist. Does nothing more than silence audio for SentenceResources matching child LocalizedTextResource
	// UUIDs.
	//
	if (!ModConfiguration.CachedDialogueBlacklist.empty())
	{
		addCallback(
			RTTI::FindTypeByName("SentenceResource"),
			[](auto& Object)
			{
				if (const auto& textResource = Object->GetMemberRefUnsafe<Ref<RTTIRefObject>>("Text"))
				{
					const bool needsPatching = std::binary_search(
						ModConfiguration.CachedDialogueBlacklist.begin(),
						ModConfiguration.CachedDialogueBlacklist.end(),
						textResource->m_UUID);

					if (needsPatching)
					{
						Object->GetMemberRefUnsafe<bool>("ShowSubtitles") = false;

						if (auto& soundResource = Object->GetMemberRefUnsafe<Ref<RTTIRefObject>>("SimpleSound"))
						{
							soundResource->GetMemberRefUnsafe<float>("DefaultVolume") = 0.0f;
							soundResource->GetMemberRefUnsafe<float>("DefaultLfeVolume") = 0.0f;
						}
					}
				}
			});
	}

	//
	// HACK: Read the fake "InternalLODRangeModifier" asset override value and forward it to the debug menu. GameViewResource
	// isn't actually present in the game files so we can remove it entirely.
	//
	if (auto itr = m_RTTIPatchesByType.find(RTTI::FindTypeByName("GameViewResource")); itr != m_RTTIPatchesByType.end())
	{
		if (!itr->second.empty())
		{
			const auto& value = std::get<RTTIValuePatch>(itr->second.front()).m_Value;

			if (!value.empty())
				std::from_chars(value.data(), value.data() + value.size(), m_CachedLODRangeModifierHack);
		}

		m_RTTIPatchesByType.erase(itr);
	}

	//
	// Global object cache lists
	//
#define RegisterCacheCallback(RTTIName, MemberName)			\
	addCallback(											\
		RTTI::FindTypeByName(RTTIName),				        \
		[](auto& Object)									\
		{													\
			auto& e = GetInstance();						\
			std::scoped_lock lock(e.m_CachedDataMutex);		\
			e.MemberName.emplace(Object);					\
		});													\
	addUnloadCallback(										\
		RTTI::FindTypeByName(RTTIName),				        \
		[](auto& Object)									\
		{													\
			auto& e = GetInstance();						\
			std::scoped_lock lock(e.m_CachedDataMutex);		\
			e.MemberName.erase(Object);						\
		});

	RegisterCacheCallback("AIFaction", m_CachedAIFactions);

	RegisterCacheCallback("SpawnSetup", m_CachedSpawnSetups);
	RegisterCacheCallback("SpawnSetupGroup", m_CachedSpawnSetups);
	RegisterCacheCallback("SpawnSetupPlaceholder", m_CachedSpawnSetups);

	RegisterCacheCallback("WeatherSetup", m_CachedWeatherSetups);
	RegisterCacheCallback("WeatherSetupDLC", m_CachedWeatherSetups);
	RegisterCacheCallback("LocalWeatherSetup", m_CachedWeatherSetups);

	RegisterCacheCallback("GraphProgramResource", m_CachedGraphPrograms);

	addCallback(
		RTTI::FindTypeByName("GraphProgramResource"),
		[](auto& Object)
		{
			GraphRuntime::OnGraphProgramLoaded(Object.GetPtr());
		});

	GraphRuntime::LoadConfig();
	Supercells::Initialize();
	Htn::Initialize();

	RegisterCacheCallback("InventoryItemResource", m_CachedInventoryItems);

#undef RegisterCacheCallback
}

bool ModCoreEvents::ShouldDumpType(const std::string& TypeName) const
{
	if (m_AssetDumpFilters.empty())
		return false;

	std::string lowered(TypeName);
	std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return std::tolower(c); });

	return std::ranges::any_of(m_AssetDumpFilters, [&](const auto& f) { return lowered.find(f) != std::string::npos; });
}

void ModCoreEvents::DumpObject(HRZ2::RTTIRefObject *Object, const char *Reason)
{
	const auto rtti = Object->GetRTTI();
	const auto depth = static_cast<size_t>(std::clamp(ModConfiguration.AssetDumpDepth, 1, 8));
	const auto text = rtti->AsCompound()->SerializeObject(Object, depth).value_or("<FAILED>");

	std::scoped_lock lock(m_AssetDumpMutex);

	FILE *f = nullptr;

	if (_wfopen_s(&f, InternalModConfig::GetModRelativePath("asset_dump.txt").c_str(), L"ab") == 0 && f)
	{
		auto header = std::format("==== [{}] {} {}\n", Reason, rtti->GetSymbolName(), Object->m_UUID);
		fwrite(header.data(), 1, header.size(), f);
		fwrite(text.data(), 1, text.size(), f);
		fwrite("\n\n", 1, 2, f);
		fclose(f);
	}
}

void ModCoreEvents::OnFinishLoadGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects)
{
	if (ModConfiguration.EnableAssetLogging)
		spdlog::info("OnFinishLoadGroup");

	HRZ2::GraphRuntime::TryInstall();
	Supercells::TryInstall();
	Htn::TryInstall();
}

void ModCoreEvents::OnBeforeUnloadGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects)
{
	if (ModConfiguration.EnableAssetLogging)
		spdlog::info("OnBeforeUnloadGroup");

	if (!m_ComponentInjects.empty() || !m_ComponentInjectRules.empty())
	{
		std::scoped_lock lock(m_InjectMutex);
		TrackInjectObjectsLocked(Objects, false);
	}

	if (!m_RefFindUUIDs.empty())
		FindReferences(Objects, false);

	if (ModConfiguration.AllowApexOverride)
		TrackApexSetups(Objects, false);

	for (auto& object : Objects)
	{
		const auto rtti = object->GetRTTI();

		if (auto itr = m_RTTIUnloadsByType.find(rtti); itr != m_RTTIUnloadsByType.end())
		{
			for (const auto& entry : itr->second)
				entry.m_Callback(object);
		}
	}
}

void ModCoreEvents::OnLoadAssetGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects)
{
	if (ModConfiguration.EnableAssetLogging)
		spdlog::info("OnLoadAssetGroup");

	for (auto& object : Objects)
	{
		const auto rtti = object->GetRTTI();

		if (ModConfiguration.EnableAssetLogging)
			spdlog::debug("Asset {}: {}", object->m_UUID, rtti->GetSymbolName());

		RTTIScanner::AddObservedType(rtti);

		// Dump before patches so the file shows original values
		if (!m_AssetDumpFilters.empty() && (ShouldDumpType(rtti->GetSymbolName()) || ShouldDumpType(std::format("{}", object->m_UUID))))
			DumpObject(object.GetPtr(), "load");

		// Apply all patches loaded from user config
		auto visitAll = [&](const auto& Entries)
		{
			auto v = [&object, &rtti](auto&& Patch)
			{
				if constexpr (std::is_same_v<std::decay_t<decltype(Patch)>, RTTIValuePatch>)
				{
					if constexpr (false)
						spdlog::debug("Patching '{}.{}' with value '{}'", rtti->GetSymbolName(), Patch.m_Path, Patch.m_Value);

					if (Patch.m_Path.starts_with('@'))
					{
						auto& src = object->GetMemberRefUnsafe<HRZ2::Ref<HRZ2::RTTIRefObject>>(Patch.m_Path.c_str() + 1);

						if (Patch.m_Value.empty())
							src = nullptr;
						else if (Patch.m_Value.starts_with('@'))
							src = object->GetMemberRefUnsafe<HRZ2::Ref<HRZ2::RTTIRefObject>>(Patch.m_Value.c_str() + 1);
						else
							spdlog::error("Error applying asset override: Invalid reference value '{}'", Patch.m_Value);
					}
					else
					{
						ValuePatchVisitor v(Patch);
						HRZ2::RTTIObjectTweaker::VisitObjectPath(object, rtti, Patch.m_Path, &v);

						if (!v.m_LastError.empty())
							spdlog::error("Error applying asset override: {}", v.m_LastError);
					}
				}
				else if constexpr (std::is_same_v<std::decay_t<decltype(Patch)>, RTTICallbackPatch>)
					Patch.m_Callback(object);
				else
					static_assert(!sizeof(decltype(Patch)), "Invalid type");
			};

			for (auto& entry : Entries)
				std::visit(v, entry);
		};

		if (auto itr = m_RTTIPatchesByType.find(rtti); itr != m_RTTIPatchesByType.end())
			visitAll(itr->second);

		if (auto itr = m_RTTIPatchesByUUID.find(object->m_UUID); itr != m_RTTIPatchesByUUID.end())
			visitAll(itr->second);
	}

	if (!m_ComponentInjects.empty() || !m_ComponentInjectRules.empty())
	{
		std::scoped_lock lock(m_InjectMutex);
		TrackInjectObjectsLocked(Objects, true);
		ApplyComponentInjectsLocked();
	}

	if (!m_RefFindUUIDs.empty())
		FindReferences(Objects, true);

	if (ModConfiguration.AllowApexOverride)
		TrackApexSetups(Objects, true);
}

const std::string& ModCoreEvents::InjectTypeName(const HRZ2::RTTIRefObject *Object)
{
	const HRZ2::RTTI *rtti = Object->GetRTTI();
	auto name = m_InjectTypeNames.find(rtti);

	if (name == m_InjectTypeNames.end())
		name = m_InjectTypeNames.emplace(rtti, rtti->GetSymbolName()).first;

	return name->second;
}

// Components that Apex spawn setups replace with their own tuned versions
static bool IsApexTuningType(const std::string& Type)
{
	static const std::unordered_set<std::string> types = {
		"DamageModifierComponentResource",
		"DamageDealtModifierComponentResource",
		"LootComponentResource",
		"CollectableRobotComponentResource",
		"HitReactionComponentResource",
		"DestructibilityResource",
		"FocusTargetComponentResource",
		"CharacterDescriptionComponentResource",
		"InitialFactsComponentResource",
		"ShaderOverridesComponentResource",
	};

	return types.contains(Type);
}

void ModCoreEvents::TrackApexSetups(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects, bool Loaded)
{
	using ComponentArray = HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>;

	std::scoped_lock lock(m_ApexMutex, m_InjectMutex);

	auto components = [](HRZ2::RTTIRefObject *Setup) -> ComponentArray *
	{
		try
		{
			return &Setup->GetMemberRefUnsafe<ComponentArray>("ComponentResources");
		}
		catch (const std::exception&)
		{
			return nullptr;
		}
	};

	auto removeInjected = [&](HRZ2::RTTIRefObject *Setup, ApexRecipient& Recipient)
	{
		if (auto array = components(Setup))
		{
			for (size_t i = 0; i < array->size(); i++)
			{
				if ((*array)[i].GetPtr() == Recipient.m_Injected)
				{
					(*array)[i] = (*array)[array->size() - 1];
					array->pop_back();
					break;
				}
			}
		}

		Recipient.m_Injected = nullptr;
	};

	if (!Loaded)
	{
		std::unordered_set<HRZ2::RTTIRefObject *> unloading;

		for (auto& object : Objects)
		{
			if (object.GetPtr())
				unloading.insert(object.GetPtr());
		}

		// Donors whose spawn setup or hackable resource goes away
		std::unordered_set<HRZ2::RTTIRefObject *> lostHackables;

		for (auto& [entity, donors] : m_ApexDonors)
		{
			std::erase_if(donors, [&](const ApexDonor& d)
			{
				if (!unloading.contains(d.m_Setup) && !unloading.contains(d.m_Hackable))
					return false;

				lostHackables.insert(d.m_Hackable);
				return true;
			});
		}

		std::erase_if(m_ApexDonors, [](const auto& e) { return e.second.empty(); });

		for (auto itr = m_ApexRecipients.begin(); itr != m_ApexRecipients.end();)
		{
			auto& [setup, recipient] = *itr;

			if (unloading.contains(setup))
			{
				itr = m_ApexRecipients.erase(itr);
				continue;
			}

			if (recipient.m_Injected && lostHackables.contains(recipient.m_Injected))
			{
				// Still referenced elsewhere? Only keep it if another donor of this entity uses the same resource.
				auto donors = m_ApexDonors.find(recipient.m_Entity);
				const bool stillDonated = donors != m_ApexDonors.end() &&
					std::ranges::any_of(donors->second, [&](const ApexDonor& d) { return d.m_Hackable == recipient.m_Injected; });

				if (!stillDonated || unloading.contains(recipient.m_Injected))
				{
					removeInjected(setup, recipient);
					spdlog::info("[apex] removed override from {} (donor unloading)", recipient.m_UUID);
				}
			}

			++itr;
		}
	}
	else
	{
		for (auto& object : Objects)
		{
			if (!object.GetPtr() || InjectTypeName(object.GetPtr()) != "SpawnSetup")
				continue;

			HRZ2::RTTIRefObject *entity = nullptr;

			try
			{
				entity = object->GetMemberRefUnsafe<HRZ2::Ref<HRZ2::RTTIRefObject>>("EntityResource").GetPtr();
			}
			catch (const std::exception&)
			{
			}

			auto array = components(object.GetPtr());

			if (!entity || !array)
				continue;

			HRZ2::RTTIRefObject *hackable = nullptr;
			bool apex = false;
			std::vector<HRZ2::RTTIRefObject *> list;

			for (size_t i = 0; i < array->size(); i++)
			{
				auto component = (*array)[i].GetPtr();

				if (!component)
					continue;

				const auto& type = InjectTypeName(component);
				bool apexSet = false;

				if (type == "HackableComponentResource")
				{
					hackable = component;
					continue;
				}

				if (type == "EntityComponentSetResource")
				{
					// The Apex variant adds a set with a dealt damage multiplier
					if (auto set = components(component))
					{
						for (size_t j = 0; j < set->size() && !apexSet; j++)
							apexSet = (*set)[j].GetPtr() && InjectTypeName((*set)[j].GetPtr()) == "DamageDealtModifierComponentResource";
					}
				}

				apex |= apexSet;

				if (!apexSet)
					list.push_back(component);
			}

			std::ranges::sort(list);

			if (hackable)
				m_ApexDonors[entity].emplace_back(ApexDonor { object.GetPtr(), hackable, std::move(list) });
			else if (apex || ModConfiguration.ApexOverrideAllVariants)
				m_ApexRecipients[object.GetPtr()] = ApexRecipient { entity, nullptr, object->m_UUID, std::move(list) };
		}
	}

	// Hand out donors to recipients that don't have one
	for (auto& [setup, recipient] : m_ApexRecipients)
	{
		if (recipient.m_Injected)
			continue;

		auto donors = m_ApexDonors.find(recipient.m_Entity);

		if (donors == m_ApexDonors.end() || donors->second.empty())
			continue;

		auto array = components(setup);

		if (!array)
			continue;

		if (ModConfiguration.ApexOverrideDebug && m_ApexDebugLogged.insert(recipient.m_UUID).second)
			LogApexCandidates(recipient, donors->second);

		// Many machines share one entity resource, and variants of a machine differ in weapons and parts. A donor fits
		// when the only differences are the per-variant tuning components (damage, loot, description...) plus node
		// graphs, and Apex-only component sets.
		const ApexDonor *best = nullptr;
		size_t bestShared = 0;
		std::string closestMismatch;

		for (const auto& donor : donors->second)
		{
			std::vector<HRZ2::RTTIRefObject *> shared, onlyApex, onlyDonor;
			std::ranges::set_intersection(recipient.m_Components, donor.m_Components, std::back_inserter(shared));
			std::ranges::set_difference(recipient.m_Components, donor.m_Components, std::back_inserter(onlyApex));
			std::ranges::set_difference(donor.m_Components, recipient.m_Components, std::back_inserter(onlyDonor));

			std::string mismatch;

			for (auto object : onlyApex)
			{
				const auto& type = InjectTypeName(object);

				if (!IsApexTuningType(type) && type != "NodeGraphComponentResource" && type != "EntityComponentSetResource")
					mismatch += std::format(" +{}", type);
			}

			for (auto object : onlyDonor)
			{
				const auto& type = InjectTypeName(object);

				if (!IsApexTuningType(type) && type != "NodeGraphComponentResource")
					mismatch += std::format(" -{}", type);
			}

			if (!mismatch.empty())
			{
				if (closestMismatch.empty() || mismatch.size() < closestMismatch.size())
					closestMismatch = std::format("{}:{}", donor.m_Setup->m_UUID, mismatch);

				continue;
			}

			if (!best || shared.size() > bestShared)
			{
				best = &donor;
				bestShared = shared.size();
			}
		}

		const auto total = recipient.m_Components.size();

		if (!best)
		{
			if (!recipient.m_Reported)
			{
				recipient.m_Reported = true;
				spdlog::info("[apex] {}: no matching donor yet (closest {})", recipient.m_UUID, closestMismatch);
			}

			continue;
		}

		auto hackable = best->m_Hackable;
		array->push_back(HRZ2::Ref<HRZ2::RTTIRefObject>(hackable));
		recipient.m_Injected = hackable;

		spdlog::info("[apex] {} can now be overridden (component {} from {}, {}/{} components shared)", recipient.m_UUID, hackable->m_UUID,
			best->m_Setup->m_UUID, bestShared, total);
	}
}

void ModCoreEvents::LogApexCandidates(const ApexRecipient& Recipient, const std::vector<ApexDonor>& Donors)
{
	struct Candidate
	{
		const ApexDonor *m_Donor;
		size_t m_Shared;
	};

	// Best donor per distinct hackable component
	std::vector<Candidate> candidates;

	for (const auto& donor : Donors)
	{
		std::vector<HRZ2::RTTIRefObject *> shared;
		std::ranges::set_intersection(Recipient.m_Components, donor.m_Components, std::back_inserter(shared));

		auto itr = std::ranges::find_if(candidates, [&](const Candidate& c) { return c.m_Donor->m_Hackable == donor.m_Hackable; });

		if (itr == candidates.end())
			candidates.emplace_back(Candidate { &donor, shared.size() });
		else if (shared.size() > itr->m_Shared)
			*itr = Candidate { &donor, shared.size() };
	}

	std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) { return a.m_Shared > b.m_Shared; });

	spdlog::info("[apex-debug] {} ({} components, {} donors, {} hackables)", Recipient.m_UUID, Recipient.m_Components.size(), Donors.size(),
		candidates.size());

	auto describe = [&](HRZ2::RTTIRefObject *Object) { return std::format("{} {}", InjectTypeName(Object), Object->m_UUID); };

	for (size_t i = 0; i < candidates.size(); i++)
	{
		const auto& c = candidates[i];

		spdlog::info("[apex-debug]   hackable {} via {}: shared {}, donor has {}", c.m_Donor->m_Hackable->m_UUID, c.m_Donor->m_Setup->m_UUID,
			c.m_Shared, c.m_Donor->m_Components.size());

		if (i >= 3)
			continue;

		std::vector<HRZ2::RTTIRefObject *> onlyRecipient, onlyDonor;
		std::ranges::set_difference(Recipient.m_Components, c.m_Donor->m_Components, std::back_inserter(onlyRecipient));
		std::ranges::set_difference(c.m_Donor->m_Components, Recipient.m_Components, std::back_inserter(onlyDonor));

		for (auto object : onlyRecipient)
			spdlog::info("[apex-debug]     apex only:  {}", describe(object));

		for (auto object : onlyDonor)
			spdlog::info("[apex-debug]     donor only: {}", describe(object));
	}
}

static bool SafeReadQword(const void *Address, uint64_t& Value)
{
	__try
	{
		Value = *static_cast<const volatile uint64_t *>(Address);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

void ModCoreEvents::FindReferences(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects, bool Loaded)
{
	std::scoped_lock lock(m_RefFindMutex);

	for (auto& object : Objects)
	{
		if (!object.GetPtr() || !m_RefFindUUIDs.contains(object->m_UUID))
			continue;

		if (Loaded)
			m_RefFindObjects[reinterpret_cast<uintptr_t>(object.GetPtr())] = object->m_UUID;
		else
			m_RefFindObjects.erase(reinterpret_cast<uintptr_t>(object.GetPtr()));
	}

	if (!Loaded || m_RefFindObjects.empty())
		return;

	auto memberAt = [](HRZ2::RTTIRefObject *Object, const HRZ2::RTTICompound *Type, size_t Offset)
	{
		std::string name = "?";
		size_t best = 0;

		Type->VisitClassMembersByInheritance(Object, [&](const HRZ2::RTTICompound::Attr& Member, void *MemberObject)
		{
			const auto offset = static_cast<size_t>(static_cast<char *>(MemberObject) - reinterpret_cast<char *>(Object));

			if (offset <= Offset && offset >= best)
			{
				best = offset;
				name = Member.m_Name;
			}

			return false;
		});

		return std::format("{}+0x{:X}", name, Offset - best);
	};

	auto report = [&](HRZ2::RTTIRefObject *Object, const HRZ2::RTTICompound *Type, size_t Offset, int Index, const HRZ2::GGUUID& Target)
	{
		auto key = std::format("{}/{}", Object->m_UUID, Target);

		if (!m_RefFindLogged.insert(key).second)
			return;

		auto where = memberAt(Object, Type, Offset);

		if (Index >= 0)
			where += std::format("[{}]", Index);

		spdlog::info("[refs] {} {} {} -> {}", Type->GetSymbolName(), Object->m_UUID, where, Target);
	};

	for (auto& object : Objects)
	{
		if (!object.GetPtr())
			continue;

		const auto type = object->GetRTTI()->AsCompound();

		if (!type)
			continue;

		const auto base = reinterpret_cast<const uint8_t *>(object.GetPtr());

		for (size_t offset = 8; offset + 8 <= type->m_Size; offset += 8)
		{
			uint64_t value = 0;

			if (!SafeReadQword(base + offset, value))
				break;

			if (auto itr = m_RefFindObjects.find(value); itr != m_RefFindObjects.end())
			{
				report(object.GetPtr(), type, offset, -1, itr->second);
				continue;
			}

			// Array<T>: int32 length, int32 capacity, T *data
			const auto length = static_cast<int32_t>(value & 0xFFFFFFFF);
			const auto capacity = static_cast<int32_t>(value >> 32);
			uint64_t data = 0;

			if (length <= 0 || length > capacity || capacity > 65536 || offset + 16 > type->m_Size || !SafeReadQword(base + offset + 8, data) || data < 0x10000)
				continue;

			for (int32_t i = 0; i < length; i++)
			{
				uint64_t element = 0;

				if (!SafeReadQword(reinterpret_cast<const void *>(data + i * 8), element))
					break;

				if (auto itr = m_RefFindObjects.find(element); itr != m_RefFindObjects.end())
					report(object.GetPtr(), type, offset, i, itr->second);
			}
		}
	}
}

void ModCoreEvents::TrackInjectObjectsLocked(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects, bool Loaded)
{
	for (size_t o = 0; o < Objects.size(); o++)
	{
		auto& object = Objects[o];

		if (!object.GetPtr())
			continue;

		const auto uuid = object->m_UUID;

		if (Loaded && !m_ComponentInjectRules.empty())
			MatchInjectRulesLocked(object.GetPtr());

		const bool used = std::ranges::any_of(m_ComponentInjects, [&](const auto& c) { return c.m_Target == uuid || c.m_Component == uuid; });

		if (!used)
			continue;

		if (Loaded)
		{
			m_InjectObjects[uuid] = object.GetPtr();
			continue;
		}

		// A component about to go away: take it back out of targets that are still loaded
		for (const auto& c : m_ComponentInjects)
		{
			if (c.m_Component != uuid)
				continue;

			auto target = m_InjectObjects.find(c.m_Target);

			if (target == m_InjectObjects.end())
				continue;

			try
			{
				auto& array = target->second->GetMemberRefUnsafe<HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>>(c.m_Field.c_str());

				for (size_t i = 0; i < array.size(); i++)
				{
					if (array[i].GetPtr() == object.GetPtr())
					{
						array[i] = array[array.size() - 1];
						array.pop_back();
						spdlog::info("[inject] removed {} from {}.{} (unloading)", uuid, c.m_Target, c.m_Field);
						break;
					}
				}
			}
			catch (const std::exception&)
			{
			}
		}

		m_InjectObjects.erase(uuid);
	}
}

void ModCoreEvents::MatchInjectRulesLocked(HRZ2::RTTIRefObject *Object)
{
	const HRZ2::RTTI *rtti = Object->GetRTTI();
	auto name = m_InjectTypeNames.find(rtti);

	if (name == m_InjectTypeNames.end())
		name = m_InjectTypeNames.emplace(rtti, rtti->GetSymbolName()).first;

	for (const auto& rule : m_ComponentInjectRules)
	{
		if (name->second != rule.m_TargetType)
			continue;

		if (m_InjectReported.insert(HRZ2::GGUUID {}).second)
			spdlog::info("[inject] first {} seen: {}", name->second, Object->m_UUID);

		try
		{
			auto& array = Object->GetMemberRefUnsafe<HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>>(rule.m_Field.c_str());
			bool contains = false;
			bool hasComponent = false;

			for (size_t i = 0; i < array.size(); i++)
			{
				if (!array[i].GetPtr())
					continue;

				contains |= array[i]->m_UUID == rule.m_TargetContains;
				hasComponent |= array[i]->m_UUID == rule.m_Component;
			}

			if (hasComponent && !contains && m_InjectReported.insert(Object->m_UUID).second)
				spdlog::info("[inject] {} {} already references {}", name->second, Object->m_UUID, rule.m_Component);

			if (!contains)
				continue;

			const bool known = std::ranges::any_of(m_ComponentInjects, [&](const auto& c)
			{
				return c.m_Target == Object->m_UUID && c.m_Component == rule.m_Component && c.m_Field == rule.m_Field;
			});

			if (!known)
			{
				m_ComponentInjects.emplace_back(ComponentInject { Object->m_UUID, rule.m_Field, rule.m_Component });
				spdlog::info("[inject] matched {} {} (references {})", name->second, Object->m_UUID, rule.m_TargetContains);
			}
		}
		catch (const std::exception& e)
		{
			spdlog::error("[inject] {} {}: {}", name->second, Object->m_UUID, e.what());
		}
	}
}

void ModCoreEvents::ApplyComponentInjectsLocked()
{
	for (const auto& c : m_ComponentInjects)
	{
		auto target = m_InjectObjects.find(c.m_Target);
		auto component = m_InjectObjects.find(c.m_Component);

		if (target == m_InjectObjects.end() || component == m_InjectObjects.end())
			continue;

		try
		{
			auto& array = target->second->GetMemberRefUnsafe<HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>>(c.m_Field.c_str());

			bool present = false;

			for (size_t i = 0; i < array.size(); i++)
				present |= array[i].GetPtr() == component->second;

			if (present)
				continue;

			array.push_back(HRZ2::Ref<HRZ2::RTTIRefObject>(component->second));
			spdlog::info("[inject] added {} to {}.{} ({} entries)", c.m_Component, c.m_Target, c.m_Field, array.size());
		}
		catch (const std::exception& e)
		{
			spdlog::error("[inject] {}.{}: {}", c.m_Target, c.m_Field, e.what());
		}
	}
}

ModCoreEvents& ModCoreEvents::GetInstance()
{
	// Yes, I'm intentionally leaking memory. There's no virtual destructor present and this
	// never gets unregistered properly.
	static auto handler = new ModCoreEvents();
	return *handler;
}
