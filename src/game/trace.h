#pragma once

// N64Recomp trace mode hooks (trace_mode = true in config/winback.us.toml).
// Records the functions each game thread entered most recently; printed by
// the debug stats thread under WINBACK_DEBUG.

#ifdef __cplusplus
extern "C" {
#endif
void winback_trace_enter(const char* func);
#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() winback_trace_enter(__func__);
#define TRACE_RETURN()
