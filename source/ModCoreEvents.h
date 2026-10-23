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

	// Apex override: give spawn setups without a HackableComponentResource the one used by another spawn setup of the
	// same entity resource
	struct ApexRecipient
	{
		HRZ2::RTTIRefObject *m_Entity = nullptr;
		HRZ2::RTTIRefObject *m_Injected = nullptr; // Hackable component
		std::vector<HRZ2::RTTIRefObject *> m_InjectedAll; // Hackable component and its node graphs
		std::vector<HRZ2::GGUUID> m_AddedHierarchy;		 // Callable mount spawn setups added to SpawnSetupHierarchy
		HRZ2::GGUUID m_UUID;
		std::vector<HRZ2::RTTIRefObject *> m_Components; // Sorted, without the Apex set
		bool m_Reported = false;
	};

	struct ApexDonor
	{
		HRZ2::RTTIRefObject *m_Setup = nullptr;
		HRZ2::RTTIRefObject *m_Hackable = nullptr;
		std::vector<HRZ2::RTTIRefObject *> m_Components; // Sorted, without the hackable component
	};

	std::unordered_map<HRZ2::RTTIRefObject *, ApexRecipient> m_ApexRecipients;
	std::unordered_map<HRZ2::RTTIRefObject *, std::vector<ApexDonor>> m_ApexDonors;
	std::unordered_set<HRZ2::GGUUID> m_CallableMountSetups;
	std::unordered_map<const void *, std::string> m_TypeNames;
	std::mutex m_ApexMutex;

	const std::string& TypeName(const HRZ2::RTTIRefObject *Object);
	void RemoveApexInjectionLocked(HRZ2::RTTIRefObject *Setup, ApexRecipient& Recipient);
	void HandOutApexDonorsLocked();
	void TrackApexSetups(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects, bool Loaded);

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

	virtual void OnFinishLoadGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects) override;
	virtual void OnBeforeUnloadGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects) override;
	virtual void OnLoadAssetGroup(const HRZ2::Array<HRZ2::Ref<HRZ2::RTTIRefObject>>& Objects) override;

	static ModCoreEvents& GetInstance();
};
