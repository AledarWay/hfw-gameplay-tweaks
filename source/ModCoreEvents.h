#pragma once

#include <mutex>
#include <unordered_set>
#include "HRZ2/Core/IStreamingSystem.h"
#include "HRZ2/Core/RTTIObjectTweaker.h"
#include "HRZ2/PCore/Common.h"

namespace HRZ2
{
	class RTTI;
	class RTTIRefObject;
}

class ModCoreEvents : public HRZ2::IStreamingSystem::Events
{
private:
	struct RTTICallbackPatch
	{
		void (*m_Callback)(HRZ2::Ref<HRZ2::RTTIRefObject>&);
	};

	struct RTTIValuePatch
	{
		HRZ2::String m_Path;
		HRZ2::String m_Value;
	};

	using RTTIPatch = std::variant<RTTICallbackPatch, RTTIValuePatch>;

	class ValuePatchVisitor : public HRZ2::RTTIObjectTweaker::SetValueVisitor
	{
	public:
		ValuePatchVisitor(const RTTIValuePatch& Patch);
		ValuePatchVisitor(const ValuePatchVisitor&) = delete;
		virtual ~ValuePatchVisitor() override = default;

		virtual void Visit(void *Object, const HRZ2::RTTI *Type) override;
		virtual EMode GetMode() override;
	};

	// Patching callbacks
	std::unordered_map<const HRZ2::RTTI *, std::vector<RTTIPatch>> m_RTTIPatchesByType;
	std::unordered_map<HRZ2::GGUUID, std::vector<RTTIPatch>> m_RTTIPatchesByUUID;
	std::unordered_map<const HRZ2::RTTI *, std::vector<RTTICallbackPatch>> m_RTTIUnloadsByType;

	// Character override
	std::optional<HRZ2::GGUUID> m_CharacterOverrideRootUUID;
	std::optional<HRZ2::GGUUID> m_CharacterOverrideVariantUUID;

	// Component injection: add a loaded component resource to another loaded object's array
	struct ComponentInject
	{
		HRZ2::GGUUID m_Target;
		std::string m_Field;
		HRZ2::GGUUID m_Component;
	};

	struct ComponentInjectRule
	{
		std::string m_TargetType;
		HRZ2::GGUUID m_TargetContains;
		std::string m_Field;
		HRZ2::GGUUID m_Component;
	};

	std::vector<ComponentInject> m_ComponentInjects;
	std::vector<ComponentInjectRule> m_ComponentInjectRules;
	std::unordered_map<const void *, std::string> m_InjectTypeNames;
	std::unordered_set<HRZ2::GGUUID> m_InjectReported;
	std::unordered_map<HRZ2::GGUUID, HRZ2::RTTIRefObject *> m_InjectObjects;
	std::mutex m_InjectMutex;

	void TrackInjectObjectsLocked(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects, bool Loaded);
	void ApplyComponentInjectsLocked();
	void MatchInjectRulesLocked(HRZ2::RTTIRefObject *Object);

	// Asset dumping
	std::vector<std::string> m_AssetDumpFilters;
	std::mutex m_AssetDumpMutex;

	// Reference finder (research)
	std::unordered_set<HRZ2::GGUUID> m_RefFindUUIDs;
	std::unordered_map<uintptr_t, HRZ2::GGUUID> m_RefFindObjects;
	std::unordered_set<std::string> m_RefFindLogged;
	std::mutex m_RefFindMutex;

	void FindReferences(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects, bool Loaded);

public:
	HRZ2::SharedMutex m_CachedDataMutex;
	std::unordered_set<HRZ2::RTTIRefObject *> m_CachedAIFactions;
	std::unordered_set<HRZ2::RTTIRefObject *> m_CachedSpawnSetups;
	std::unordered_set<HRZ2::RTTIRefObject *> m_CachedWeatherSetups;
	std::unordered_set<HRZ2::RTTIRefObject *> m_CachedGraphPrograms;
	std::unordered_set<HRZ2::RTTIRefObject *> m_CachedInventoryItems;
	float m_CachedLODRangeModifierHack = std::numeric_limits<float>::max();

	ModCoreEvents();
	ModCoreEvents(const ModCoreEvents&) = delete;
	virtual ~ModCoreEvents() = default;

	void RegisterParsedPatches();
	void RegisterHardcodedPatches();

	bool ShouldDumpType(const std::string& TypeName) const;
	void DumpObject(HRZ2::RTTIRefObject *Object, const char *Reason);

	virtual void OnFinishLoadGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects) override;
	virtual void OnBeforeUnloadGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects) override;
	virtual void OnLoadAssetGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects) override;

	static ModCoreEvents& GetInstance();
};
