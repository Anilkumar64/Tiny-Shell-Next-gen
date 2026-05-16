#pragma once

// Compatibility shim only. The old server-local SecureChannel was a plaintext
// magic-byte stub and must not be resurrected. Include the real channel from
// common/ explicitly so server-local quoted includes cannot shadow it.
#include "../common/SecureChannel.h"
