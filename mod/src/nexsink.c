// nexsink.dll - replace a host's shared WASAPI stream with our own exclusive one.
//
// Why this shape: an exclusive-mode stream preempts every shared stream on the
// same endpoint. Measured on MOONDROP MM3A - opening exclusive while the client
// played flipped its session from ACTIVE to inactive and it never rebuilt.
// So "let the client play shared, tap ReleaseBuffer, forward to exclusive" cannot
// work. The client must not own a real stream at all.
//
// Instead we hand the client a fake IAudioClient:
//
//   client -> Initialize(SHARED, float32 @ track rate)   -> we open EXCLUSIVE s24
//   client -> GetBufferSize / GetCurrentPadding          -> our ring buffer
//   client -> GetService(IAudioRenderClient)             -> our proxy writer
//   client -> Start                                      -> we start the real stream
//
// The proxy preserves the client's sample representation in a ring. A drain
// thread converts only when the accepted device format differs. Native-format
// streams bypass SRC and quantisation. An optional opening fade is a separate
// processing choice, not evidence of bit-perfect delivery.
//
// Configuration is re-read from NEX_CFG (default hook.cfg) on mtime change:
//   enable=1        engage the sink at all (0 = pure pass-through, for A/B)
//   bits=24         output depth: 16 or 24
//   rate=0          output rate, 0 = follow whatever the client asks for
//   buffer=2048     frames reported to the client via GetBufferSize (0 = auto)

#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

/* ---- vtable slots ------------------------------------------------------- */
#define SLOT_QI              0
#define SLOT_ADDREF          1
#define SLOT_RELEASE         2

#define DEV_ACTIVATE         3   /* IMMDevice */

#define AC_INITIALIZE        3
#define AC_GETBUFFERSIZE     4
#define AC_GETSTREAMLATENCY  5
#define AC_GETCURRENTPADDING 6
#define AC_ISFORMATSUPPORTED 7
#define AC_GETMIXFORMAT      8
#define AC_GETDEVICEPERIOD   9
#define AC_START             10
#define AC_STOP              11
#define AC_RESET             12
#define AC_SETEVENTHANDLE    13
#define AC_GETSERVICE        14
#define AC3_INITSHARED       20  /* IAudioClient3::InitializeSharedAudioStream */

#define RC_GETBUFFER         3
#define RC_RELEASEBUFFER     4

#define CLK_GETFREQUENCY     3
#define CLK_GETPOSITION      4
#define CLK_GETCHARACTER     5


/* The implementation is split by responsibility, but compiled as one
   translation unit: the regression harness includes this file directly and
   reaches into the static state (the patch table, the sink, the resampler), so
   the pieces are textual includes rather than separate objects with a header
   between them. Order matters - each file may call what an earlier one
   defines. */
#include "sink_core.inc"        /* config, logging, state, vtable bookkeeping */
#include "sink_dsp.inc"         /* ring buffer, sample conversion, resampler  */
#include "sink_device.inc"      /* drain thread, faults, fake client objects  */
#include "sink_audiohost.inc"   /* optional shared-memory out-of-process sink */
#include "sink_lifecycle.inc"   /* device generations, stop and teardown      */
#include "sink_endpoint.inc"    /* format negotiation, migration, engage      */
#include "sink_hooks.inc"       /* IAudioClient/IMMDevice hooks, watchdog     */
