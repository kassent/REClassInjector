#pragma once
#include <ci/API.h>

namespace ci {
    const ManagedObject& PrepareObjectAPI(uintptr_t createInstanceAddress, uintptr_t createArrayAddress,
                                          uintptr_t createActionAddress, uintptr_t retainAddress,
                                          uintptr_t releaseAddress, uintptr_t fastAllocateStringAddress);
    const ResourceManager& PrepareResourceAPI(uintptr_t deserializeAddress, void** resourceManagerAddress,
                                              uintptr_t createUserDataAddress, void** resourceTypeInfoAddress,
                                              uintptr_t createResourceAddress, const ManagedObject& objectAPI);
} // namespace ci
