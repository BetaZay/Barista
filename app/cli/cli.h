#pragma once

namespace barista::cli
{
// Select terminal handling before QApplication attempts to connect to a display.
bool Requested(int argc, char** argv);
int Run(int argc, char** argv);
}
