// Reading the engine's switches from the environment.
#pragma once
#include <cstdlib>

// An empty switch has the same meaning as one that is absent.
inline const char* getenv_nonempty(const char* name) {
    const char* value = getenv(name);
    return value && *value ? value : nullptr;
}
