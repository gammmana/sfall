/*
 *    sfall
 *    Copyright (C) 2008-2025  The sfall team
 *
 */

#pragma once

#include "Module.h"

namespace sfall
{

class MetaCursorHighlight : public Module {
public:
	const char* name() { return "MetaCursorHighlight"; }
	void init();
	void exit();

	static void OnSfallGlobalSet(__int64 varId, int value);
};

}
