#ifndef VOICEMOD_SHIM_SESSIONS_H
#define VOICEMOD_SHIM_SESSIONS_H

#include <audiopolicy.h>

/* Install fake virtual-mic sessions into the (shared) IAudioSessionManager2 vtable. */
void sessions_patch(IAudioSessionManager2Vtbl *vtbl);

/* Event Voicemod hands its driver with IOCTL 0x1d2007; the real driver signals it. */
void sessions_set_driver_event(HANDLE event);

#endif
