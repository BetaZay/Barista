#include "drh/version.h"

#ifndef BARISTA_VERSION_STRING
#define BARISTA_VERSION_STRING "0.1.0"
#endif

namespace barista::drh
{
std::string_view version()
{
	return BARISTA_VERSION_STRING;
}
}
