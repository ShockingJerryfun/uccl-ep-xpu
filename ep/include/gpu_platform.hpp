#pragma once

// USE_METAX is the platform switch used by every transport. Keep the old
// URMA-specific spelling as a source compatibility alias while deployments
// migrate their build scripts.
#if defined(USE_URMA_METAX) && !defined(USE_METAX)
#define USE_METAX
#endif
