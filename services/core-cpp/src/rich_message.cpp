// rich_message.cpp — translation unit for the Rich Messages engine.
//
// The engine is header-only (all functions inline in
// rich_message.hpp) so that translation units including the
// dispatcher keep linking without a new link dependency. This file
// exists so the header is compiled standalone once, and so the
// production target lists the engine explicitly.
#include "rich_message.hpp"
