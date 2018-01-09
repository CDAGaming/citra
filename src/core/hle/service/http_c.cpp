// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/service/http_c.h"

namespace Service {
namespace HTTP {

static const ResultCode ERROR_CONTEXT_ERROR(0xD8A0A066);
static const ResultCode RESULT_DOWNLOADPENDING(0xd840a02b);

bool HTTP_C::ContextExists(u32 context_id, IPC::RequestParser& rp) const {
    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return false;
    }
    return true;
}

void HTTP_C::Initialize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x1, 1, 4);
    const u32 shmem_size = rp.Pop<u32>();
    const u32 process_header = rp.Pop<u32>();
    rp.Pop<u32>(); // unused
    shared_memory = rp.PopObject<Kernel::SharedMemory>();
    if (shared_memory) {
        shared_memory->name = "HTTP_C:shared_memory";
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, shmem_size=%u", shmem_size);
}

void HTTP_C::CreateContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x2, 2, 2);
    const u32 url_size = rp.Pop<u32>();

    Context context;
    context.url.resize(url_size);
    context.method = rp.PopEnum<RequestMethod>();

    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();
    buffer.Read(&context.url[0], 0, url_size);
    contexts[++context_counter] = context;

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(context_counter);

    LOG_WARNING(Service_HTTP, "called, url_size=%u, url=%s", url_size, context.url.c_str());
}

void HTTP_C::CloseContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x3, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;

    contexts.erase(context_id);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::GetDownloadSizeState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x6, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;
    Context& context = contexts[context_id];

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(context.current_offset);
    rb.Push<u32>(context.GetResponseContentLength());

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::InitializeConnectionSession(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x8, 1, 2);
    const u32 context_id = rp.Pop<u32>();
    rp.Pop<u32>(); // process_id = 0x20
    rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;

    contexts[context_id].initialized = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::BeginRequest(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x9, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;

    Context& context = contexts[context_id];

    switch (context.method) {
    case RequestMethod::Get: {
        context.response = cpr::Get(context.url, context.request_headers);
    } break;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::ReceiveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xB, 2, 2);
    const u32 context_id = rp.Pop<u32>();
    u32 buffer_size = rp.Pop<u32>();
    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    if (!ContextExists(context_id, rp))
        return;

    Context& context = contexts[context_id];
    u32 size = std::min(buffer_size, context.GetResponseContentLength() - context.current_offset);
    buffer.Write(&context.response.text[context.current_offset], 0, size);
    context.current_offset += size;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push((context.current_offset < context.GetResponseContentLength()) ? RESULT_DOWNLOADPENDING
                                                                          : RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::ReceiveDataTimeout(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xC, 4, 2);
    const u32 context_id = rp.Pop<u32>();
    const u32 buffer_size = rp.Pop<u32>();
    const u64 timeout = rp.Pop<u64>();
    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    if (!ContextExists(context_id, rp))
        return;

    Context& context = contexts[context_id];
    context.timeout = timeout;
    u32 size = std::min(buffer_size, context.GetResponseContentLength() - context.current_offset);
    buffer.Write(&context.response.text[context.current_offset], 0, size);
    context.current_offset += size;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push((context.current_offset < context.GetResponseContentLength()) ? RESULT_DOWNLOADPENDING
                                                                          : RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::SetProxyDefault(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xE, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;

    contexts[context_id].proxy_default = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::AddRequestHeader(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x11, 3, 4);
    const u32 context_id = rp.Pop<u32>();
    const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();
    std::string name(name_buffer.begin(), name_buffer.end());
    std::string value(value_size, '\0');
    value_buffer.Read(&value[0], 0, value_size);

    if (!ContextExists(context_id, rp))
        return;
    contexts[context_id].request_headers[name] = value;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::GetResponseStatusCode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x22, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(contexts[context_id].GetResponseStatusCode());

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::GetResponseStatusCodeTimeout(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x23, 3, 0);
    const u32 context_id = rp.Pop<u32>();
    const u64 timeout = rp.Pop<u64>();

    if (!ContextExists(context_id, rp))
        return;
    contexts[context_id].timeout = timeout;
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(contexts[context_id].GetResponseStatusCode());

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::SetSSLOpt(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x2B, 2, 0);
    const u32 context_id = rp.Pop<u32>();
    const u32 ssl_options = rp.Pop<u32>();

    if (!ContextExists(context_id, rp))
        return;
    contexts[context_id].ssl_options = ssl_options;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u, ssl_options=0x%X", context_id, ssl_options);
}

void HTTP_C::SetKeepAlive(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x2B, 2, 0);
    const u32 context_id = rp.Pop<u32>();
    const bool keep_alive = rp.Pop<bool>();

    if (!ContextExists(context_id, rp))
        return;
    contexts[context_id].keep_alive = keep_alive;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u, keep_alive=%u", context_id, keep_alive);
}

HTTP_C::HTTP_C() : ServiceFramework("http:C", 14) {
    static const FunctionInfo functions[] = {
        {0x00010044, &HTTP_C::Initialize, "Initialize"},
        {0x00020082, &HTTP_C::CreateContext, "CreateContext"},
        {0x00030040, &HTTP_C::CloseContext, "CloseContext"},
        {0x00040040, nullptr, "CancelConnection"},
        {0x00050040, nullptr, "GetRequestState"},
        {0x00060040, &HTTP_C::GetDownloadSizeState, "GetDownloadSizeState"},
        {0x00070040, nullptr, "GetRequestError"},
        {0x00080042, &HTTP_C::InitializeConnectionSession, "InitializeConnectionSession"},
        {0x00090040, &HTTP_C::BeginRequest, "BeginRequest"},
        {0x000A0040, nullptr, "BeginRequestAsync"},
        {0x000B0082, &HTTP_C::ReceiveData, "ReceiveData"},
        {0x000C0102, &HTTP_C::ReceiveDataTimeout, "ReceiveDataTimeout"},
        {0x000D0146, nullptr, "SetProxy"},
        {0x000E0040, &HTTP_C::SetProxyDefault, "SetProxyDefault"},
        {0x000F00C4, nullptr, "SetBasicAuthorization"},
        {0x00100080, nullptr, "SetSocketBufferSize"},
        {0x001100C4, &HTTP_C::AddRequestHeader, "AddRequestHeader"},
        {0x001200C4, nullptr, "AddPostDataAscii"},
        {0x001300C4, nullptr, "AddPostDataBinary"},
        {0x00140082, nullptr, "AddPostDataRaw"},
        {0x00150080, nullptr, "SetPostDataType"},
        {0x001600C4, nullptr, "SendPostDataAscii"},
        {0x00170144, nullptr, "SendPostDataAsciiTimeout"},
        {0x001800C4, nullptr, "SendPostDataBinary"},
        {0x00190144, nullptr, "SendPostDataBinaryTimeout"},
        {0x001A0082, nullptr, "SendPostDataRaw"},
        {0x001B0102, nullptr, "SendPOSTDataRawTimeout"},
        {0x001C0080, nullptr, "SetPostDataEncoding"},
        {0x001D0040, nullptr, "NotifyFinishSendPostData"},
        {0x001E00C4, nullptr, "GetResponseHeader"},
        {0x001F0144, nullptr, "GetResponseHeaderTimeout"},
        {0x00200082, nullptr, "GetResponseData"},
        {0x00210102, nullptr, "GetResponseDataTimeout"},
        {0x00220040, &HTTP_C::GetResponseStatusCode, "GetResponseStatusCode"},
        {0x002300C0, &HTTP_C::GetResponseStatusCodeTimeout, "GetResponseStatusCodeTimeout"},
        {0x00240082, nullptr, "AddTrustedRootCA"},
        {0x00250080, nullptr, "AddDefaultCert"},
        {0x00260080, nullptr, "SelectRootCertChain"},
        {0x002700C4, nullptr, "SetClientCert"},
        {0x002B0080, &HTTP_C::SetSSLOpt, "SetSSLOpt"},
        {0x002C0080, nullptr, "SetSSLClearOpt"},
        {0x002D0000, nullptr, "CreateRootCertChain"},
        {0x002E0040, nullptr, "DestroyRootCertChain"},
        {0x002F0082, nullptr, "RootCertChainAddCert"},
        {0x00300080, nullptr, "RootCertChainAddDefaultCert"},
        {0x00310080, nullptr, "RootCertChainRemoveCert"},
        {0x00320084, nullptr, "OpenClientCertContext"},
        {0x00330040, nullptr, "OpenDefaultClientCertContext"},
        {0x00340040, nullptr, "CloseClientCertContext"},
        {0x00350186, nullptr, "SetDefaultProxy"},
        {0x00360000, nullptr, "ClearDNSCache"},
        {0x00370080, &HTTP_C::SetKeepAlive, "SetKeepAlive"},
        {0x003800C0, nullptr, "SetPostDataTypeSize"},
        {0x00390000, nullptr, "Finalize"},
    };
    RegisterHandlers(functions);
}

HTTP_C::~HTTP_C() {
    shared_memory = nullptr;
}

void InstallInterfaces(SM::ServiceManager& service_manager) {
    std::make_shared<HTTP_C>()->InstallAsService(service_manager);
}

} // namespace HTTP
} // namespace Service
