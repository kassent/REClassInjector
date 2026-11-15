#pragma once

namespace ci {
    // Install startup hooks on the EXE's first CreateWindowEx call, not in DllMain.
    // Success means the hooks are ready; type publication and provider notification await VM initialization.
    bool InstallTypeInjectionHooks();
}
