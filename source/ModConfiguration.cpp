#include <spdlog/sinks/basic_file_sink.h>
#include <toml++/toml.h>
#include "HRZ2/DebugUI/LogWindow.h"
#include "ModConfiguration.h"

namespace InternalModConfig
{
	InternalModConfig::GlobalSettings& GetModifiableSettings()
	{
		static GlobalSettings configuration;
		return configuration;
	}

	std::filesystem::path& GetModifiableRootDirectory()
	{
		static std::filesystem::path rootDirectory;
		return rootDirectory;
	}
}

const InternalModConfig::GlobalSettings& ModConfiguration = InternalModConfig::GetModifiableSettings();

namespace InternalModConfig
{
	void ParseSettings(GlobalSettings& s, const toml::table& Table);
	void PostProcessSettings(GlobalSettings& s);

	void Initialize(const std::filesystem::path& RootDirectory)
	{
		GetModifiableRootDirectory() = RootDirectory;

		if (!InitializeConfigurationFile())
			throw std::runtime_error("Failed to load configuration file.");

		// Main logger (debug UI and log sink)
		if (auto logLevel = spdlog::level::debug; logLevel != spdlog::level::off)
		{
			std::vector<spdlog::sink_ptr> sinks;

			auto debugUISink = std::make_shared<HRZ2::DebugUI::LogWindow::LogSink>();
			sinks.emplace_back(std::move(debugUISink));

			if constexpr (true)
			{
				auto logFileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(GetModRelativePath("mod_log.txt").string(), true);
				sinks.emplace_back(std::move(logFileSink));
			}

			auto logger = std::make_shared<spdlog::logger>("main_logger", sinks.begin(), sinks.end());
			logger->set_level(logLevel);
			logger->flush_on(spdlog::level::debug);
			logger->set_pattern("[%H:%M:%S] [%l] %v");

			spdlog::set_default_logger(std::move(logger));
		}

		if (!Offsets::Initialize())
			throw std::runtime_error("Failed to initialize offsets. The game likely updated and is no longer supported.");

		if (!Hooks::Initialize())
			throw std::runtime_error("Failed to initialize hooks.");
	}

	bool InitializeConfigurationFile()
	{
		auto tryParseTomlFile = [](GlobalSettings& Settings, const std::filesystem::path& Path)
		{
			toml::table table;

			try
			{
				table = toml::parse_file(Path.string());
			}
			catch (const toml::parse_error&)
			{
				return false;
			}

			ParseSettings(Settings, table);
			return true;
		};

		// mod_config.ini takes precedence
		if (!tryParseTomlFile(GetModifiableSettings(), GetModRelativePath("mod_config.ini")))
			return false;

		// Then check for mod_*.ini files in the game directory
		for (const auto& entry : std::filesystem::directory_iterator(GetModRelativePath("")))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".ini")
				continue;

			if (!entry.path().filename().string().starts_with("mod_") || entry.path().filename().string() == "mod_config.ini")
				continue;

			if (!tryParseTomlFile(GetModifiableSettings(), entry.path()))
				return false;
		}

		PostProcessSettings(GetModifiableSettings());
		return true;
	}

	std::filesystem::path GetModRelativePath(const std::string_view& PartialPath)
	{
		auto path = GetModifiableRootDirectory();
		path.append(PartialPath);

		return path;
	}

	void ParseSettings(GlobalSettings& o, const toml::table& Table)
	{
#define PARSE_TOML_MEMBER(obj, x) o.x = (*obj)[#x].value_or(decltype(o.x) {})
#define PARSE_TOML_HOTKEY(obj, x) o.Hotkeys.x = (*obj)[#x].value_or(-1)

		// [General]
		if (auto general = Table["General"].as_table())
		{
			PARSE_TOML_MEMBER(general, EnableDebugMenu);
			PARSE_TOML_MEMBER(general, EnableAssetLogging);
			PARSE_TOML_MEMBER(general, EnableAssetOverrides);
			PARSE_TOML_MEMBER(general, DebugMenuFontScale);
			PARSE_TOML_MEMBER(general, AssetDumpTypes);
			PARSE_TOML_MEMBER(general, AssetDumpDepth);
			PARSE_TOML_MEMBER(general, AssetFindReferences);
			PARSE_TOML_MEMBER(general, GraphScanKeywords);

			// Older configs had this under [General]
			if (auto v = (*general)["KeepSupercellsAfterAether"].value<bool>())
				o.KeepSupercellsAfterAether = *v;
		}

		if (o.DebugMenuFontScale <= 0.0f)
			o.DebugMenuFontScale = 1.0f;

		// [Gameplay]
		if (auto gameplay = Table["Gameplay"].as_table())
		{
			PARSE_TOML_MEMBER(gameplay, UnlockMapBoundaries);
			PARSE_TOML_MEMBER(gameplay, UnlockWorldMapMenu);
			PARSE_TOML_MEMBER(gameplay, UnlockMountRestrictions);
			PARSE_TOML_MEMBER(gameplay, UnlockEntitlementExtras);
			PARSE_TOML_MEMBER(gameplay, UnlockPhotomodeRestrictions);
			PARSE_TOML_MEMBER(gameplay, UnlockAIvsAIDamage);
			PARSE_TOML_MEMBER(gameplay, UnlockUltraHardRestrictions);
			PARSE_TOML_MEMBER(gameplay, UnlockTrapAndTripwireLimits);
			PARSE_TOML_MEMBER(gameplay, NoCampfireSaveBlackScreen);

			PARSE_TOML_MEMBER(gameplay, EnableFreePerks);
			PARSE_TOML_MEMBER(gameplay, EnableFreeCrafting);
			PARSE_TOML_MEMBER(gameplay, EnableGodMode);
			PARSE_TOML_MEMBER(gameplay, EnableInfiniteClipAmmo);
			PARSE_TOML_MEMBER(gameplay, EnableInfiniteOverrideTimer);
			PARSE_TOML_MEMBER(gameplay, EnableAutoNeutralFaction);

			PARSE_TOML_MEMBER(gameplay, IncreaseInventoryStacks);
			PARSE_TOML_MEMBER(gameplay, InventoryStackMultiplier);

			PARSE_TOML_MEMBER(gameplay, DisableCameraMagnetism);
			PARSE_TOML_MEMBER(gameplay, DisableFocusMagnetism);

			PARSE_TOML_MEMBER(gameplay, ForceCustomUltraHard);
			PARSE_TOML_MEMBER(gameplay, ForceNoShaderLoadingScreen);

			if (auto v = (*gameplay)["KeepSupercellsAfterAether"].value<bool>())
				o.KeepSupercellsAfterAether = *v;

			PARSE_TOML_MEMBER(gameplay, AllowAggressiveFlyingMountOverride);
			PARSE_TOML_MEMBER(gameplay, AllowApexOverride);
			PARSE_TOML_MEMBER(gameplay, ApexOverrideAllVariants);
			PARSE_TOML_MEMBER(gameplay, ApexOverrideDebug);
			PARSE_TOML_MEMBER(gameplay, CalledFlyingMountFights);
		}

		auto parseStringArray = [](auto Node, std::vector<std::string>& Target)
		{
			if (auto a = Node.as_array())
			{
				for (const auto& e : *a)
				{
					if (auto v = e.value<std::string>())
						Target.emplace_back(*v);
				}
			}
		};

		// [FlyingMountAI]
		{
			const auto t = Table["FlyingMountAI"].as_table();
			auto get = [&](const char *Name, auto Default)
			{
				return t ? (*t)[Name].value_or(Default) : Default;
			};

			auto& f = o.FlyingMountAI;
			f.Sunwing = get("Sunwing", true);
			f.Waterwing = get("Waterwing", true);
			f.SkipIdleInAir = get("SkipIdleInAir", true);
			f.SkipIdleLanded = get("SkipIdleLanded", true);
			f.SkipIdleInWater = get("SkipIdleInWater", true);
			f.SkipGoToHackerLanded = get("SkipGoToHackerLanded", false);
			f.SkipSwimNearHacker = get("SkipSwimNearHacker", false);
			f.SkipWholeLandedBranch = get("SkipWholeLandedBranch", false);
			f.NoLandingNextToTarget = get("NoLandingNextToTarget", true);
			f.FlyingRangedAttack = get("FlyingRangedAttack", true);
			f.NoEvadeInWater = get("NoEvadeInWater", true);
			f.OutOfWaterDelay = get("OutOfWaterDelay", 3.0);
			f.LookAtThreatSeconds = get("LookAtThreatSeconds", 3.0);
			f.RangedAttackWindow = get("RangedAttackWindow", 6.0);
			f.RangedAttackCooldown = get("RangedAttackCooldown", 12.0);
		}

		// [GraphDebug]
		if (auto graphDebug = Table["GraphDebug"].as_table())
		{
			parseStringArray((*graphDebug)["WatchGraphs"], o.WatchGraphs);
			parseStringArray((*graphDebug)["TraceGraphs"], o.TraceGraphs);
			PARSE_TOML_MEMBER(graphDebug, TraceSupercells);
			parseStringArray((*graphDebug)["WatchFacts"], o.WatchFacts);
			PARSE_TOML_MEMBER(graphDebug, NativeFactHooks);
			PARSE_TOML_MEMBER(graphDebug, HtnDebug);
		}

		// [Supercells]
		if (auto supercells = Table["Supercells"].as_table())
		{
			o.SupercellIncludeAllLocations = (*supercells)["IncludeAllLocations"].value_or(false);
			o.SupercellIgnoreQuestBlocks = (*supercells)["IgnoreQuestBlocks"].value_or(false);
			o.SupercellUseCustomLocations = (*supercells)["UseCustomLocations"].value_or(false);

			if (auto a = (*supercells)["DisabledLocations"].as_array())
			{
				for (const auto& e : *a)
				{
					if (auto v = e.value<int64_t>())
						o.SupercellDisabledLocations.emplace_back(static_cast<int>(*v));
				}
			}
		}

		// [[SupercellLocation]]
		if (auto locationArray = Table["SupercellLocation"].as_array())
		{
			locationArray->for_each(
				[&](const toml::table& t)
				{
					SupercellLocation l;
					l.Enabled = t["Enable"].value_or(true);
					l.X = t["X"].value_or(0.0);
					l.Y = t["Y"].value_or(0.0);

					if (auto z = t["Z"].value<double>())
					{
						l.Z = *z;
						l.HasZ = true;
					}

					l.Inner = t["Inner"].value_or(0.0f);
					l.Outer = t["Outer"].value_or(0.0f);
					l.Template = t["Template"].value_or(-1);
					o.SupercellLocations.emplace_back(l);
				});
		}

		// [[ComponentInject]]
		if (auto injects = Table["ComponentInject"].as_array())
		{
			injects->for_each(
				[&](const toml::table& t)
				{
					ComponentInject c;
					c.Enabled = t["Enable"].value_or(true);
					c.Target = std::string(t["Target"].value_or(""));
					c.TargetType = std::string(t["TargetType"].value_or(""));
					c.TargetContains = std::string(t["TargetContains"].value_or(""));
					c.Field = std::string(t["Field"].value_or("ComponentResources"));
					c.Component = std::string(t["Component"].value_or(""));
					o.ComponentInjects.emplace_back(std::move(c));
				});
		}

		// [[SlotProbe]]
		if (auto probes = Table["SlotProbe"].as_array())
		{
			probes->for_each(
				[&](const toml::table& t)
				{
					SlotProbe p;
					p.Enabled = t["Enable"].value_or(true);
					p.Slot = std::string(t["Slot"].value_or(""));
					p.Force = std::string(t["Force"].value_or(""));
					p.DescribeEntity = t["DescribeEntity"].value_or(false);
					p.DumpResult = t["DumpResult"].value_or(false);
					parseStringArray(t["Graphs"], p.Graphs);
					o.SlotProbes.emplace_back(std::move(p));
				});
		}

		// [[HTNPatch]]
		if (auto htnArray = Table["HTNPatch"].as_array())
		{
			htnArray->for_each(
				[&](const toml::table& t)
				{
					HtnPatch h;
					h.Enabled = t["Enable"].value_or(true);
					h.Domain = std::string(t["Domain"].value_or(""));

					if (auto v = t["Offset"].value<std::string>())
						h.Offset = *v;
					else if (auto i = t["Offset"].as_integer())
						h.Offset = std::to_string(i->get());

					if (auto v = t["Value"].value<std::string>())
						h.Value = *v;
					else if (auto i = t["Value"].as_integer())
						h.Value = std::to_string(i->get());

					o.HtnPatches.emplace_back(std::move(h));
				});
		}

		// [[FactOverride]]
		if (auto factOverrideArray = Table["FactOverride"].as_array())
		{
			factOverrideArray->for_each(
				[&](const toml::table& t)
				{
					FactOverride f;
					f.Enabled = t["Enable"].value_or(true);
					f.Fact = std::string(t["Fact"].value_or(""));
					f.Type = std::string(t["Type"].value_or(""));
					f.WriteBack = t["WriteBack"].value_or(false);

					// Value may be written as a string, bool or number
					if (auto v = t["Value"].value<std::string>())
						f.Value = *v;
					else if (auto b = t["Value"].value<bool>())
						f.Value = *b ? "true" : "false";
					else if (auto i = t["Value"].as_integer())
						f.Value = std::to_string(i->get());
					else if (auto d = t["Value"].as_floating_point())
						f.Value = std::to_string(d->get());

					if (auto graph = t["Graphs"].value<std::string>())
						f.Graphs.emplace_back(*graph);
					else
						parseStringArray(t["Graphs"], f.Graphs);

					o.FactOverrides.emplace_back(std::move(f));
				});
		}

		// [Hotkeys]
		if (auto hotkeys = Table["Hotkeys"].as_table())
		{
			PARSE_TOML_HOTKEY(hotkeys, ToggleDebugUI);
			PARSE_TOML_HOTKEY(hotkeys, TogglePauseGameLogic);
			PARSE_TOML_HOTKEY(hotkeys, ToggleFreeCamera);
			PARSE_TOML_HOTKEY(hotkeys, ToggleNoclip);
			PARSE_TOML_HOTKEY(hotkeys, IncreaseTimescale);
			PARSE_TOML_HOTKEY(hotkeys, DecreaseTimescale);
			PARSE_TOML_HOTKEY(hotkeys, ToggleTimescale);
			PARSE_TOML_HOTKEY(hotkeys, SpawnEntity);
			PARSE_TOML_HOTKEY(hotkeys, QuickSave);
			PARSE_TOML_HOTKEY(hotkeys, QuickLoad);
			PARSE_TOML_HOTKEY(hotkeys, ToggleAIProcessing);
			PARSE_TOML_HOTKEY(hotkeys, TogglePauseTimeOfDay);
		}

		// [AssetOverride]
		if (auto overrideArray = Table["AssetOverride"].as_array())
		{
			auto parseAssetOverride = [](const toml::table *Table, bool Enable)
			{
				AssetOverride o;

				o.Enabled = Enable;
				PARSE_TOML_MEMBER(Table, ObjectUUIDs);
				PARSE_TOML_MEMBER(Table, ObjectTypes);
				PARSE_TOML_MEMBER(Table, Path);
				PARSE_TOML_MEMBER(Table, Value);

				return o;
			};

			overrideArray->for_each(
				[&](const toml::table& t)
				{
					const bool enabled = t["Enable"].value_or(false);

					if (auto overridePatchArray = t["Patch"].as_array())
					{
						for (const auto& patchEntry : *overridePatchArray)
							o.AssetOverrides.emplace_back(parseAssetOverride(patchEntry.as_table(), enabled));
					}
				});
		}

		// [CharacterOverride]
		if (auto overrideArray = Table["CharacterOverride"].as_array())
		{
			overrideArray->for_each(
				[&](const toml::table& t)
				{
					o.CharacterOverrides.emplace_back(
						HRZ2::GGUUID::Parse(t["RootUUID"].value_or("")),
						HRZ2::GGUUID::Parse(t["VariantUUID"].value_or("")));
				});
		}

		// [DamageDealtModifiers]
		if (auto overrideArray = Table["DamageDealtModifiers"]["Overrides"].as_array())
		{
			o.DamageModifierOverrides.clear();
			o.DamageModifierOverrides.reserve(overrideArray->size());

			overrideArray->for_each([&](const toml::array& a)
			{
				o.DamageModifierOverrides.emplace_back(HRZ2::GGUUID::Parse(a[0].value_or("")), a[1].value_or(1.0f));
			});
		}

		// [CoreObjectCache]
		auto parseCoreObjectCacheTable = [&Table](const std::string_view& Name, auto& TargetVector)
		{
			if (auto cachedObjectArray = Table["CoreObjectCache"][Name].as_array())
			{
				TargetVector.clear();
				TargetVector.reserve(cachedObjectArray->size());

				cachedObjectArray->for_each(
					[&](const toml::array& a)
					{
						TargetVector.emplace_back(
							a.size() > 0 ? HRZ2::GGUUID::Parse(a[0].value_or("")) : HRZ2::GGUUID {},
							a.size() > 1 ? HRZ2::GGUUID::Parse(a[1].value_or("")) : HRZ2::GGUUID {},
							a.size() > 2 ? a[2].value_or("") : "");
					});

				std::stable_sort(TargetVector.begin(), TargetVector.end());
			}
		};

		parseCoreObjectCacheTable("CachedSpawnSetups", o.CachedSpawnSetups);
		parseCoreObjectCacheTable("CachedWeatherSetups", o.CachedWeatherSetups);
		parseCoreObjectCacheTable("CachedInventoryItems", o.CachedInventoryItems);
		parseCoreObjectCacheTable("CachedDialogueBlacklist", o.CachedDialogueBlacklist);
		parseCoreObjectCacheTable("CachedRoots", o.CachedRoots);

#undef PARSE_TOML_HOTKEY
#undef PARSE_TOML_MEMBER
	}

	void PostProcessSettings(GlobalSettings& s)
	{
		// clang-format off
		if (!s.EnableAssetOverrides)
			s.AssetOverrides.clear();

		// UnlockMapBoundaries
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockMapBoundaries,
			.ObjectUUIDs = "BA5AB7E8-E4CC-0D41-88CD-A9681E5DF4A3", // GraphProgramResource
			.Path = "EntryPoints[2].EntryPoint",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockMapBoundaries,
			.ObjectTypes = "OutOfBoundsQueryComponentResource",
			.Path = "OutOfBoundsAreaTags",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockMapBoundaries,
			.ObjectUUIDs = "E8E912B6-17E3-8344-959D-A867A9F467A1", // RiddenMountInAirAvoidanceResource
			.Path = "PhysicsCollisionLayer", // 2DE5F274-1BF7-6E40-8732-2FFF2AC61DA5 - map barriers and settlements
			.Value = "No Collision",
		});

		// UnlockWorldMapMenu
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockWorldMapMenu,
			.ObjectTypes = "DiscoverableArea",
			.Path = "InitialState",
			.Value = "Completed",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockWorldMapMenu,
			.ObjectTypes = "DiscoverableAreaResource",
			.Path = "FogOfWarRevealType",
			.Value = "Full",
		});

		// UnlockMountRestrictions
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockMountRestrictions,
			.ObjectTypes = "HorseControllerComponentResource,HorseCallComponentResource,GroundMountCallComponentResource,AirMountCallComponentResource",
			.Path = "HorseNotAllowedNavMeshAreaTags",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockMountRestrictions,
			.ObjectTypes = "RiddenMountControllerComponentResource",
			.Path = "DisallowedNavMeshAreaTags",
			.Value = "",
		});

		// UnlockPhotomodeRestrictions
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockPhotomodeRestrictions,
			.ObjectTypes = "PhotoModeResource",
			.Path = "MaxCameraRadius",
			.Value = "1000000",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockPhotomodeRestrictions,
			.ObjectUUIDs = "9D995414-CEF7-6830-AAFA-87CCF64E0448", // GraphProgramResource
			.Path = "EntryPoints[0].EntryPoint",
			.Value = "EntryPoint_EQ12_01_ArmadilloFight_ConditionGraph_019fa88caf88265ff2c20624746b9a87_0_Evaluate",
		});

		// UnlockAIvsAIDamage
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockAIvsAIDamage,
			.ObjectTypes = "AIManagerResourceGame",
			.Path = "@IgnoredAttackEventTagFilter",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockAIvsAIDamage,
			.ObjectTypes = "AIManagerResourceGame",
			.Path = "@IgnoredAttacksOnAIFilter",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockAIvsAIDamage,
			.ObjectTypes = "ReduceIncomingAIDamageComponentResource",
			.Path = "@NotDamagedRecentlyDamageResource", // NotDamagedRecentlyDamageResource = DefaultDamageResource
			.Value = "@DefaultDamageResource",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockAIvsAIDamage,
			.ObjectTypes = "ReduceIncomingAIDamageResource",
			.Path = "DenyKillingBlow",
			.Value = "false",
		});

		// UnlockUltraHardRestrictions
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockUltraHardRestrictions,
			.ObjectTypes = "DifficultyManagerResource",
			.Path = "LockedDifficulties",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockUltraHardRestrictions,
			.ObjectTypes = "DataSourceSettingsResource",
			.Path = "@UltraHardDifficulty",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockUltraHardRestrictions,
			.ObjectTypes = "DifficultyPresetResource",
			.Path = "IsConcentrationModifierAllowed",
			.Value = "true",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockUltraHardRestrictions,
			.ObjectTypes = "DifficultyPresetResource",
			.Path = "IsAutoConcentrationAllowed",
			.Value = "true",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockUltraHardRestrictions,
			.ObjectTypes = "DifficultyPresetResource",
			.Path = "IsAutoHealAllowed",
			.Value = "true",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockUltraHardRestrictions,
			.ObjectTypes = "DifficultyPresetResource",
			.Path = "IsTrajectoryAssistAllowed",
			.Value = "true",
		});

		// NoCampfireSaveBlackScreen: screen effect events of the campfire save sequence that fade to and hold black
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.NoCampfireSaveBlackScreen,
			.ObjectUUIDs = "D5C403E9-37B7-C247-9485-F85C0FBAD22F,6E917529-55A2-D04B-AE6D-ABCD2357B5B2,F6882B8E-4B23-DD4C-887D-CF9E3062F8E0,"
						   "3978F20B-B300-4649-83F8-2CFA10FD20FE,A4D81F39-9E5B-5747-B514-A20EC157086F,2570F6E2-7D29-7935-BDBA-9960A83341CD",
			.Path = "BlendWeight",
			.Value = "0",
		});

		// UnlockTrapAndTripwireLimits
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockTrapAndTripwireLimits,
			.ObjectUUIDs = "2AC1A900-3547-9F47-877E-F46601C3675F", // PlaceableItemLimiterComponentResource
			.Path = "PlaceableItemLimits",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.UnlockTrapAndTripwireLimits,
			.ObjectUUIDs = "38EE0EBE-FA3E-734F-A09D-DD211FA175B6", // HUDVitalStatusComponentResource
			.Path = "@TrapAndTripwireLimitReachedMessage",
			.Value = "@LowAmmoMessage",
		});

		// EnableFreePerks
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.EnableFreePerks,
			.ObjectTypes = "PerkLevel",
			.Path = "Cost",
			.Value = "0",
		});

		// EnableFreeCrafting
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.EnableFreeCrafting,
			.ObjectTypes = "CraftableItemComponentResource,CraftableItemSettings",
			.Path = "Ingredients.Requirements",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.EnableFreeCrafting,
			.ObjectTypes = "CraftableItemComponentResource,CraftableItemSettings",
			.Path = "DefaultIngredients.Requirements",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.EnableFreeCrafting,
			.ObjectTypes = "CraftableItemComponentResource,CraftableItemSettings",
			.Path = "ConditionalIngredients",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.EnableFreeCrafting,
			.ObjectTypes = "TransactionRequirementsContainer",
			.Path = "Requirements",
			.Value = "",
		});

		// EnableInfiniteOverrideTimer
		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = s.EnableInfiniteOverrideTimer,
			.ObjectTypes = "HackableComponentResource",
			.Path = "BaseHackDuration",
			.Value = "10000000",
		});

		const bool characterOverrideEnabled = !s.CharacterOverrides.empty();

		s.AssetOverrides.emplace_back(AssetOverride // Delete armor model parts
		{
			.Enabled = characterOverrideEnabled,
			.ObjectTypes = "OutfitStreamingData",
			.Path = "ModelParts",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride // Delete Aloy's hair
		{
			.Enabled = characterOverrideEnabled,
			.ObjectUUIDs = "1040F13B-33B7-163B-AD58-3F4F03F1489E",
			.Path = "Poses",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride // Delete skin/face tints and masks (vanilla uuidref override)
		{
			.Enabled = characterOverrideEnabled,
			.ObjectUUIDs = "20F405C6-E25F-7B41-B988-58EDC2756C8A",
			.Path = "ShaderOverrides.ShaderTextureOverrides",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = characterOverrideEnabled,
			.ObjectUUIDs = "20F405C6-E25F-7B41-B988-58EDC2756C8A",
			.Path = "ShaderOverrides.ShaderVariableOverrides",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride // Delete skin/face tints and masks (armor override)
		{
			.Enabled = characterOverrideEnabled,
			.ObjectTypes = "PlayerOutfitTheme",
			.Path = "ShaderEffects",
			.Value = "",
		});

		s.AssetOverrides.emplace_back(AssetOverride
		{
			.Enabled = characterOverrideEnabled,
			.ObjectTypes = "PlayerOutfitTheme",
			.Path = "DefaultShaderEffect.ShaderOverrides",
			.Value = "",
		});
		// clang-format on
	}
}
