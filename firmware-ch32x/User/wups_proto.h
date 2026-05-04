/*
 * Local stub that pulls in the shared Web3 Pi UPS wire-protocol header
 * from Web3-Pi-UPS/common/. Kept as a thin wrapper so main.c includes
 * "wups_proto.h" rather than a relative path, and so future build-system
 * tweaks (e.g. adding common/ to the include search path) localize here.
 */
#ifndef CH32X_WUPS_PROTO_H
#define CH32X_WUPS_PROTO_H

#include "../../common/protocol.h"

#endif /* CH32X_WUPS_PROTO_H */
