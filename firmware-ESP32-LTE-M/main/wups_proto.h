/*
 * Pulls the shared Web3 Pi UPS wire-protocol header into the ESP32-S3
 * firmware. Kept as a thin wrapper so source files can `#include
 * "wups_proto.h"` rather than a relative path, and so any future tweak
 * to the include path is localized here.
 */
#pragma once

#include "../../common/protocol.h"
