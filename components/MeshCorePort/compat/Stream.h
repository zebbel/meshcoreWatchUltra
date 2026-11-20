
#pragma once

// Upstream Identity.h and Utils.h declare methods using Arduino Stream&.
// Packet and radio interfaces only need the type declaration.
// This is NOT an implementation of Arduino Stream; it must be replaced
// before compiling upstream Identity.cpp or Utils.cpp.
class Stream;
