#pragma once

#include "ClumpModelInfo.h"

class CMloModelInfo : public CClumpModelInfo
{
public:
	float drawDist;
	int firstInstance;
	int lastInstance;
public:
	CMloModelInfo(void) : CClumpModelInfo(MITYPE_MLO), drawDist(0.0f), firstInstance(0), lastInstance(0) {}
	void ConstructClump();

	virtual void RcWriteThis(base::cRelocatableChunkWriter &writer);
	virtual void RcWriteEmpty(base::cRelocatableChunkWriter &writer);

	static base::cRelocatableChunkClassInfo msClassInfo;
	static CMloModelInfo msClassInstance;
};