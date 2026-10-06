// hooks_dwrite.h — font enumeration filtering through DirectWrite.
#pragma once

namespace ghost {

// Arms DirectWrite font-collection filtering from the profile's `fonts`
// allow-list. A profile without that key leaves DirectWrite untouched, so this
// is safe to call unconditionally.
void install_dwrite_hooks();

}  // namespace ghost
