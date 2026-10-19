#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>
#include "Hooking/Memory.h"
#include "ModConfiguration.h"
#include "DefendArea.h"

namespace DefendArea
{
	namespace
	{
		struct Section
		{
			uintptr_t m_Start = 0;
			size_t m_Size = 0;
		};

		Section FindSection(uintptr_t Base, const char *Name)
		{
			auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(Base + reinterpret_cast<const IMAGE_DOS_HEADER *>(Base)->e_lfanew);
			auto section = IMAGE_FIRST_SECTION(nt);

			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
			{
				if (strncmp(reinterpret_cast<const char *>(section->Name), Name, IMAGE_SIZEOF_SHORT_NAME) == 0)
					return { Base + section->VirtualAddress, section->Misc.VirtualSize };
			}

			return {};
		}

		template<typename T>
		const uint8_t *FindValue(const Section& In, T Value, size_t Alignment, const uint8_t *From = nullptr)
		{
			auto p = From ? From : reinterpret_cast<const uint8_t *>(In.m_Start);
			const auto end = reinterpret_cast<const uint8_t *>(In.m_Start + In.m_Size - sizeof(T));

			for (; p <= end; p += Alignment)
			{
				if (memcmp(p, &Value, sizeof(T)) == 0)
					return p;
			}

			return nullptr;
		}

		// Locates the vtable of a polymorphic class from its MSVC RTTI type descriptor name
		uintptr_t *FindVtable(std::string_view MangledName)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			const auto data = FindSection(base, ".data");
			const auto rdata = FindSection(base, ".rdata");

			if (!data.m_Start || !rdata.m_Start)
				return nullptr;

			// TypeDescriptor: vftable pointer, spare, then the name
			const auto dataBegin = reinterpret_cast<const char *>(data.m_Start);
			const auto dataEnd = dataBegin + data.m_Size;
			const auto name = std::search(dataBegin, dataEnd, MangledName.begin(), MangledName.end());

			if (name == dataEnd || name[MangledName.size()] != '\0')
				return nullptr;

			const auto typeDescriptor = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(name) - 16 - base);

			// CompleteObjectLocator: signature 1, offset, cdOffset, pTypeDescriptor, pClassDescriptor, pSelf
			for (auto p = FindValue(rdata, typeDescriptor, 4); p; p = FindValue(rdata, typeDescriptor, 4, p + 4))
			{
				auto col = reinterpret_cast<const uint32_t *>(p - 12);

				if (col[0] != 1 || col[1] != 0 || col[5] != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(col) - base))
					continue;

				// The vtable is preceded by a pointer to its locator
				if (auto meta = FindValue(rdata, reinterpret_cast<uintptr_t>(col), 8))
					return reinterpret_cast<uintptr_t *>(const_cast<uint8_t *>(meta) + 8);
			}

			return nullptr;
		}

		void SkipDefendAreaUpdate(void *)
		{
		}

		// SpawnCommand::Apply(this, ?, ?, entities)
		using ApplyFn = void (*)(void *, void *, void *, void *, void *, void *);
		ApplyFn OriginalAreaRestrictionApply = nullptr;
		std::mutex AreaRestrictionMutex;

		void SkipDefendSpawnCommand(void *, void *, void *, void *, void *, void *)
		{
		}

		// Apply with an empty Areas list (offset 0x60) so only DisallowedAreas (0x70) are restricted
		void AreaRestrictionApplyDisallowedOnly(void *This, void *A2, void *A3, void *A4, void *A5, void *A6)
		{
			std::scoped_lock lock(AreaRestrictionMutex);

			auto& areaCount = *reinterpret_cast<int32_t *>(static_cast<uint8_t *>(This) + 0x60);
			const auto count = areaCount;

			areaCount = 0;
			OriginalAreaRestrictionApply(This, A2, A3, A4, A5, A6);
			areaCount = count;
		}

		bool PatchSlot(const char *Class, size_t Slot, const void *Replacement, void **Original = nullptr)
		{
			auto vtable = FindVtable(Class);

			if (!vtable)
			{
				spdlog::error("[leash] {} vtable not found", Class);
				return false;
			}

			if (Original)
				*Original = reinterpret_cast<void *>(vtable[Slot]);

			const auto value = reinterpret_cast<uintptr_t>(Replacement);
			Memory::Patch(reinterpret_cast<uintptr_t>(&vtable[Slot]), reinterpret_cast<const uint8_t *>(&value), sizeof(value));

			spdlog::info("[leash] patched {} slot {}", Class, Slot);
			return true;
		}
	}

	void TryInstall()
	{
		static bool installed = false;

		if (installed || !ModConfiguration.UnlockSpawnAreaRestrictions)
			return;

		installed = true;

		// Slot 7 updates the agent's world state from its DefendAreaUserComponent
		PatchSlot(".?AVAIHTNPlannerDaemonDefendArea@@", 7, reinterpret_cast<const void *>(&SkipDefendAreaUpdate));

		// Slot 6 applies a spawn command to the spawned entities: defend areas and navmesh area restrictions (the
		// boundary the pathfinder won't cross)
		PatchSlot(".?AVDefendSpawnCommand@@", 6, reinterpret_cast<const void *>(&SkipDefendSpawnCommand));
		PatchSlot(".?AVAreaRestrictionSpawnCommand@@", 6, reinterpret_cast<const void *>(&AreaRestrictionApplyDisallowedOnly),
			reinterpret_cast<void **>(&OriginalAreaRestrictionApply));
	}
}
