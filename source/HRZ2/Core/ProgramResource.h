#pragma once

#include "RTTIRefObject.h"

namespace HRZ2
{
	class RTTIHandle
	{
	public:
		void *m_StaticTypePtr;
		String m_StaticTypeName;
		void *m_VirtualResource;
	};

	class ProgramParameter
	{
	public:
		String m_Name;
		RTTIHandle m_Type;
		uint32_t m_FlagsAndIndex;
		uint32_t m_UUIDHash;
	};
	static_assert(sizeof(ProgramParameter) == 0x28);

	class ProgramParameterList
	{
	public:
		Array<ProgramParameter> m_Parameters;
		char _pad0[0x10];
		Array<Ref<RTTIRefObject>> m_DefaultSoftLinkedObjects;
		Array<Ref<RTTIRefObject>> m_DefaultHardLinkedObjects;
		Array<GGUUID> m_DefaultUUIDRefs;
		char _pad1[0x18];
	};
	assert_offset(ProgramParameterList, m_DefaultSoftLinkedObjects, 0x20);
	assert_offset(ProgramParameterList, m_DefaultHardLinkedObjects, 0x30);
	assert_offset(ProgramParameterList, m_DefaultUUIDRefs, 0x40);
	static_assert(sizeof(ProgramParameterList) == 0x68);

	class ProgramResourceEntryPoint
	{
	public:
		ProgramParameterList m_InputParameters;
		ProgramParameterList m_OutputParameters;
		String m_EntryPoint;
		char _pad[0x8];
	};
	static_assert(sizeof(ProgramResourceEntryPoint) == 224);

	class ProgramResource : public RTTIRefObject
	{
	public:
		Array<ProgramResourceEntryPoint> m_EntryPoints;
	};
}
