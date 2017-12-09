// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "core/hle/ipc.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/service/mic_u.h"

namespace Service {
namespace MIC {

enum class Encoding : u8 {
    PCM8 = 0,
    PCM16 = 1,
    PCM8Signed = 2,
    PCM16Signed = 3,
};

enum class SampleRate : u8 {
    SampleRate32730 = 0,
    SampleRate16360 = 1,
    SampleRate10910 = 2,
    SampleRate8180 = 3
};

static Kernel::SharedPtr<Kernel::Event> buffer_full_event;
static Kernel::SharedPtr<Kernel::SharedMemory> shared_memory;
static u8 mic_gain = 0;
static bool mic_power = false;
static bool is_sampling = false;
static bool allow_shell_closed;
static bool clamp = false;
static Encoding encoding;
static SampleRate sample_rate;
static s32 audio_buffer_offset;
static u32 audio_buffer_size;
static bool audio_buffer_loop;

/**
 * MIC::MapSharedMem service function
 *  Inputs:
 *      0 : Header Code[0x00010042]
 *      1 : Shared-mem size
 *      2 : CopyHandleDesc
 *      3 : Shared-mem handle
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void MapSharedMem(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x01, 1, 2};
    const u32 size = rp.Pop<u32>();
    const Kernel::Handle mem_handle = rp.PopHandle();

    shared_memory = Kernel::g_handle_table.Get<Kernel::SharedMemory>(mem_handle);
    if (shared_memory) {
        shared_memory->name = "MIC_U:shared_memory";
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_MIC, "called, size=0x%X, mem_handle=0x%08X", size, mem_handle);
}

/**
 * MIC::UnmapSharedMem service function
 *  Inputs:
 *      0 : Header Code[0x00020000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void UnmapSharedMem(Interface* self) {
    IPC::RequestBuilder rb{Kernel::GetCommandBuffer(), 0x02, 1, 0};
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "called");
}

/**
 * MIC::StartSampling service function
 *  Inputs:
 *      0 : Header Code[0x00030140]
 *      1 : Encoding
 *      2 : SampleRate
 *      3 : Base offset for audio data in sharedmem
 *      4 : Size of the audio data in sharedmem
 *      5 : Loop at end of buffer
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void StartSampling(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x03, 5, 0};

    encoding = static_cast<Encoding>(rp.Pop<u8>());
    sample_rate = static_cast<SampleRate>(rp.Pop<u8>());
    audio_buffer_offset = static_cast<s32>(rp.Pop<u32>());
    audio_buffer_size = rp.Pop<u32>();
    audio_buffer_loop = rp.Pop<bool>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    is_sampling = true;
    LOG_WARNING(Service_MIC, "(STUBBED) called, encoding=%u, sample_rate=%u, "
                             "audio_buffer_offset=%d, audio_buffer_size=%u, audio_buffer_loop=%u",
                static_cast<u32>(encoding), static_cast<u32>(sample_rate), audio_buffer_offset,
                audio_buffer_size, audio_buffer_loop);
}

/**
 * MIC::AdjustSampling service function
 *  Inputs:
 *      0 : Header Code[0x00040040]
 *      1 : SampleRate
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void AdjustSampling(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x04, 1, 0};
    sample_rate = static_cast<SampleRate>(rp.Pop<u8>());

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "(STUBBED) called, sample_rate=%u", static_cast<u32>(sample_rate));
}

/**
 * MIC::StopSampling service function
 *  Inputs:
 *      0 : Header Code[0x00050000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void StopSampling(Interface* self) {
    IPC::RequestBuilder rb{Kernel::GetCommandBuffer(), 0x05, 1, 0};
    rb.Push(RESULT_SUCCESS);
    is_sampling = false;
    LOG_WARNING(Service_MIC, "(STUBBED) called");
}

/**
 * MIC::IsSampling service function
 *  Inputs:
 *      0 : Header Code[0x00060000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : 0 = sampling, non-zero = sampling
 */
static void IsSampling(Interface* self) {
    IPC::RequestBuilder rb{Kernel::GetCommandBuffer(), 0x06, 2, 0};
    rb.Push(RESULT_SUCCESS);
    rb.Push<bool>(is_sampling);
    LOG_WARNING(Service_MIC, "(STUBBED) called");
}

/**
 * MIC::GetBufferFullEvent service function
 *  Inputs:
 *      0 : Header Code[0x00070000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      3 : Event handle
 */
static void GetBufferFullEvent(Interface* self) {
    IPC::RequestBuilder rb{Kernel::GetCommandBuffer(), 0x07, 1, 2};
    rb.Push(RESULT_SUCCESS);
    rb.PushCopyHandles(Kernel::g_handle_table.Create(buffer_full_event).Unwrap());
    LOG_WARNING(Service_MIC, "(STUBBED) called");
}

/**
 * MIC::SetGain service function
 *  Inputs:
 *      0 : Header Code[0x00080040]
 *      1 : Gain
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SetGain(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x08, 1, 0};
    mic_gain = rp.Pop<u8>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "(STUBBED) called, mic_gain=%u", mic_gain);
}

/**
 * MIC::GetGain service function
 *  Inputs:
 *      0 : Header Code[0x00090000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : Gain
 */
static void GetGain(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x09, 0, 0};

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u8>(mic_gain);
    LOG_WARNING(Service_MIC, "(STUBBED) called");
}

/**
 * MIC::SetPower service function
 *  Inputs:
 *      0 : Header Code[0x000A0040]
 *      1 : Power (0 = off, 1 = on)
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SetPower(Interface* self) {
    IPC::RequestParser rp{ Kernel::GetCommandBuffer(), 0x0A, 1, 0};
    mic_power = rp.Pop<bool>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "(STUBBED) called, mic_power=%u", mic_power);
}

/**
 * MIC::GetPower service function
 *  Inputs:
 *      0 : Header Code[0x000B0000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : Power
 */
static void GetPower(Interface* self) {
    IPC::RequestBuilder rb{Kernel::GetCommandBuffer(), 0x0B, 2, 0};
    rb.Push(RESULT_SUCCESS);
    rb.Push<u8>(mic_power);
    LOG_WARNING(Service_MIC, "(STUBBED) called");
}

/**
 * MIC::SetIirFilterMic service function
 *  Inputs:
 *      0 : Header Code[0x000C0042]
 *      1 : Size
 *      2 : MappedBuffer descriptor
 *      3 : Pointer to IIR Filter Data
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SetIirFilterMic(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x0C, 1, 2};
    const u32 size = rp.Pop<u32>();
    const Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "(STUBBED) called, size=0x%X, buffer=0x%08X", size);
}

/**
 * MIC::SetClamp service function
 *  Inputs:
 *      0 : Header Code[0x000D0040]
 *      1 : Clamp (0 = don't clamp, non-zero = clamp)
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SetClamp(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x0D, 1, 0};
    clamp = rp.Pop<bool>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "(STUBBED) called, clamp=%u", clamp);
}

/**
 * MIC::GetClamp service function
 *  Inputs:
 *      0 : Header Code[0x000E0000]
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : Clamp (0 = don't clamp, non-zero = clamp)
 */
static void GetClamp(Interface* self) {
    IPC::RequestBuilder rb{Kernel::GetCommandBuffer(), 0x0E, 2, 0};
    rb.Push(RESULT_SUCCESS);
    rb.Push<bool>(clamp);
    LOG_WARNING(Service_MIC, "(STUBBED) called");
}

/**
 * MIC::SetAllowShellClosed service function
 *  Inputs:
 *      0 : Header Code[0x000F0040]
 *      1 : Sampling allowed while shell closed (0 = disallow, non-zero = allow)
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SetAllowShellClosed(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x0F, 1, 0};
    allow_shell_closed = rp.Pop<bool>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
    LOG_WARNING(Service_MIC, "(STUBBED) called, allow_shell_closed=%u", allow_shell_closed);
}

/**
 * MIC_U::SetClientVersion service function
 *  Inputs:
 *      0 : Header Code[0x00100040]
 *      1 : Used SDK Version
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SetClientVersion(Interface* self) {
    IPC::RequestParser rp{Kernel::GetCommandBuffer(), 0x10, 1, 0};

    const u32 version = rp.Pop<u32>();
    self->SetVersion(version);

    LOG_WARNING(Service_MIC, "(STUBBED) called, version: 0x%08X", version);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);
}

const Interface::FunctionInfo FunctionTable[] = {
    {0x00010042, MapSharedMem, "MapSharedMem"},
    {0x00020000, UnmapSharedMem, "UnmapSharedMem"},
    {0x00030140, StartSampling, "StartSampling"},
    {0x00040040, AdjustSampling, "AdjustSampling"},
    {0x00050000, StopSampling, "StopSampling"},
    {0x00060000, IsSampling, "IsSampling"},
    {0x00070000, GetBufferFullEvent, "GetBufferFullEvent"},
    {0x00080040, SetGain, "SetGain"},
    {0x00090000, GetGain, "GetGain"},
    {0x000A0040, SetPower, "SetPower"},
    {0x000B0000, GetPower, "GetPower"},
    {0x000C0042, SetIirFilterMic, "SetIirFilterMic"},
    {0x000D0040, SetClamp, "SetClamp"},
    {0x000E0000, GetClamp, "GetClamp"},
    {0x000F0040, SetAllowShellClosed, "SetAllowShellClosed"},
    {0x00100040, SetClientVersion, "SetClientVersion"},
};

MIC_U::MIC_U() {
    Register(FunctionTable);
    shared_memory = nullptr;
    buffer_full_event =
        Kernel::Event::Create(Kernel::ResetType::OneShot, "MIC_U::buffer_full_event");
    mic_gain = 0;
    mic_power = false;
    is_sampling = false;
    clamp = false;
}

MIC_U::~MIC_U() {
    shared_memory = nullptr;
    buffer_full_event = nullptr;
}

} // namespace MIC
} // namespace Service
