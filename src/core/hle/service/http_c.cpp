// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <map>
#include <string>
#include <cpr/cpr.h>
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/service/http_c.h"

namespace Service {
namespace HTTP {

static const ResultCode ERROR_CONTEXT_ERROR = // 0xD8A0A066
    ResultCode(static_cast<ErrorDescription>(102), ErrorModule::HTTP, ErrorSummary::InvalidState,
               ErrorLevel::Permanent);
static const ResultCode RESULT_DOWNLOADPENDING = // 0xD840A02B
    ResultCode(static_cast<ErrorDescription>(43), ErrorModule::HTTP, ErrorSummary::WouldBlock,
               ErrorLevel::Permanent);

enum class RequestMethod : u8 {
    Get = 0x1,
    Post = 0x2,
    Head = 0x3,
    Put = 0x4,
    Delete = 0x5,
};

struct Context {
    cpr::Url url;
    cpr::Header request_header;
    RequestMethod method{RequestMethod::Get};
    bool initialized = false;
    bool proxy_default = false;
    bool keep_alive = false;
    u32 ssl_options = 0;
    u32 current_offset = 0;
    u64 timeout = 0;
    cpr::Response response;

    u32 GetResponseStatusCode() const {
        return response.status_code;
    }
    u32 GetResponseContentLength() const {
        try {
            const std::string length = response.header.at("Content-Length");
            return std::stoi(length);
        } catch (...) {
            return 0;
        }
    }
    cpr::VerifySsl SslOptions() const {
        static const u32 disable_verify = 0x200; // TODO(mailwl): bitfield value from ssl:C
        return cpr::VerifySsl{(ssl_options & disable_verify) != disable_verify};
    }
};

class HTTP_C::Impl {
public:
    ~Impl();
    void Initialize(Kernel::HLERequestContext& ctx);
    void CreateContext(Kernel::HLERequestContext& ctx);
    void CloseContext(Kernel::HLERequestContext& ctx);
    void GetDownloadSizeState(Kernel::HLERequestContext& ctx);
    void InitializeConnectionSession(Kernel::HLERequestContext& ctx);
    void BeginRequest(Kernel::HLERequestContext& ctx);
    void ReceiveData(Kernel::HLERequestContext& ctx);
    void ReceiveDataTimeout(Kernel::HLERequestContext& ctx);
    void SetProxyDefault(Kernel::HLERequestContext& ctx);
    void AddRequestHeader(Kernel::HLERequestContext& ctx);
    void GetResponseHeader(Kernel::HLERequestContext& ctx);
    void GetResponseStatusCode(Kernel::HLERequestContext& ctx);
    void GetResponseStatusCodeTimeout(Kernel::HLERequestContext& ctx);
    void SetSSLOpt(Kernel::HLERequestContext& ctx);
    void SetKeepAlive(Kernel::HLERequestContext& ctx);

private:
    Kernel::SharedPtr<Kernel::SharedMemory> shared_memory = nullptr;
    std::unordered_map<u32, Context> contexts;
    u32 context_counter{0};
};

HTTP_C::Impl::~Impl() {
    shared_memory = nullptr;
}

void HTTP_C::Impl::Initialize(Kernel::HLERequestContext& ctx) {
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

void HTTP_C::Impl::CreateContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x2, 2, 2);
    const u32 url_size = rp.Pop<u32>();

    Context context;
    context.url.resize(url_size);
    context.method = rp.PopEnum<RequestMethod>();

    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();
    buffer.Read(&context.url[0], 0, url_size);

    contexts[++context_counter] = context;

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(context_counter);
    rb.PushMappedBuffer(buffer);

    LOG_WARNING(Service_HTTP, "called, url_size=%u, url=%s", url_size, context.url.c_str());
}

void HTTP_C::Impl::CloseContext(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x3, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    if (contexts.find(context_id) == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    contexts.erase(context_id);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::GetDownloadSizeState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x6, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(3, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(context->second.current_offset);
    rb.Push<u32>(context->second.GetResponseContentLength());

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::InitializeConnectionSession(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x8, 1, 2);
    const u32 context_id = rp.Pop<u32>();
    rp.Pop<u32>(); // process_id = 0x20
    rp.Pop<u32>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.initialized = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::BeginRequest(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x9, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    switch (context->second.method) {
    case RequestMethod::Get:
        context->second.response = cpr::Get(context->second.url, context->second.request_header,
                                            context->second.SslOptions());
        break;
    default:
        UNIMPLEMENTED();
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::ReceiveData(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xB, 2, 2);
    const u32 context_id = rp.Pop<u32>();
    const u32 buffer_size = rp.Pop<u32>();
    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    const u32 size = std::min(buffer_size, context->second.GetResponseContentLength() -
                                               context->second.current_offset);
    buffer.Write(&context->second.response.text[context->second.current_offset], 0, size);
    context->second.current_offset += size;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(context->second.current_offset < context->second.GetResponseContentLength()
                ? RESULT_DOWNLOADPENDING
                : RESULT_SUCCESS);
    rb.PushMappedBuffer(buffer);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::ReceiveDataTimeout(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xC, 4, 2);
    const u32 context_id = rp.Pop<u32>();
    const u32 buffer_size = rp.Pop<u32>();
    const u64 timeout = rp.Pop<u64>();
    Kernel::MappedBuffer& buffer = rp.PopMappedBuffer();

    auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.timeout = timeout;
    const u32 size = std::min(buffer_size, context->second.GetResponseContentLength() -
                                               context->second.current_offset);
    buffer.Write(&context->second.response.text[context->second.current_offset], 0, size);
    context->second.current_offset += size;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(context->second.current_offset < context->second.GetResponseContentLength()
                ? RESULT_DOWNLOADPENDING
                : RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::SetProxyDefault(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xE, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.proxy_default = true;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u", context_id);
}

void HTTP_C::Impl::AddRequestHeader(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x11, 3, 4);
    const u32 context_id = rp.Pop<u32>();
    const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();
    const std::string name(reinterpret_cast<const char*>(name_buffer.data()), name_size - 1);
    std::string value(value_size - 1, '\0');
    value_buffer.Read(&value[0], 0, value_size - 1);

    auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.request_header[name] = value;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(RESULT_SUCCESS);
    rb.PushMappedBuffer(value_buffer);

    LOG_WARNING(Service_HTTP, "called, name=%s, value=%s, context_id=%u", name.c_str(),
                value.c_str(), context_id);
}

void HTTP_C::Impl::GetResponseHeader(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x1E, 3, 4);
    const u32 context_id = rp.Pop<u32>();
    const u32 name_size = rp.Pop<u32>();
    const u32 value_size = rp.Pop<u32>();
    const std::vector<u8> name_buffer = rp.PopStaticBuffer();
    Kernel::MappedBuffer& value_buffer = rp.PopMappedBuffer();
    const std::string name(reinterpret_cast<const char*>(name_buffer.data()), name_size - 1);

    auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    const std::string value = context->second.response.header[name] + '\0';

    value_buffer.Write(value.c_str(), 0, value.size());

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 2);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(value.size());
    rb.PushMappedBuffer(value_buffer);

    LOG_WARNING(Service_HTTP,
                "called, name=%s, name_size=%u, value=%s, value_size=%u, context_id=%u",
                name.c_str(), name_size, value.c_str(), value_size, context_id);
}

void HTTP_C::Impl::GetResponseStatusCode(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x22, 1, 0);
    const u32 context_id = rp.Pop<u32>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    const u32 status_code = context->second.GetResponseStatusCode();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(status_code);

    LOG_WARNING(Service_HTTP, "called, context_id=%u, status_code=%u", context_id, status_code);
}

void HTTP_C::Impl::GetResponseStatusCodeTimeout(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x23, 3, 0);
    const u32 context_id = rp.Pop<u32>();
    const u64 timeout = rp.Pop<u64>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.timeout = timeout;
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(contexts[context_id].GetResponseStatusCode());

    LOG_WARNING(Service_HTTP, "called, context_id=%u, timeout=%llu", context_id, timeout);
}

void HTTP_C::Impl::SetSSLOpt(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x2B, 2, 0);
    const u32 context_id = rp.Pop<u32>();
    const u32 ssl_options = rp.Pop<u32>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.ssl_options = ssl_options;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u, ssl_options=0x%X", context_id, ssl_options);
}

void HTTP_C::Impl::SetKeepAlive(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x37, 2, 0);
    const u32 context_id = rp.Pop<u32>();
    const bool keep_alive = rp.Pop<bool>();

    const auto context = contexts.find(context_id);
    if (context == contexts.end()) {
        IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
        rb.Push(ERROR_CONTEXT_ERROR);
        LOG_ERROR(Service_HTTP, "called, context_id=%u not found", context_id);
        return;
    }
    context->second.keep_alive = keep_alive;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_WARNING(Service_HTTP, "called, context_id=%u, keep_alive=%u", context_id, keep_alive);
}

HTTP_C::HTTP_C() : ServiceFramework("http:C", 32), pimpl{new Impl{}} {
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
        {0x001E00C4, &HTTP_C::GetResponseHeader, "GetResponseHeader"},
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

void HTTP_C::Initialize(Kernel::HLERequestContext& ctx) {
    pimpl->Initialize(ctx);
}

void HTTP_C::CreateContext(Kernel::HLERequestContext& ctx) {
    pimpl->CreateContext(ctx);
}

void HTTP_C::CloseContext(Kernel::HLERequestContext& ctx) {
    pimpl->CloseContext(ctx);
}

void HTTP_C::GetDownloadSizeState(Kernel::HLERequestContext& ctx) {
    pimpl->GetDownloadSizeState(ctx);
}

void HTTP_C::InitializeConnectionSession(Kernel::HLERequestContext& ctx) {
    pimpl->InitializeConnectionSession(ctx);
}

void HTTP_C::BeginRequest(Kernel::HLERequestContext& ctx) {
    pimpl->BeginRequest(ctx);
}

void HTTP_C::ReceiveData(Kernel::HLERequestContext& ctx) {
    pimpl->ReceiveData(ctx);
}

void HTTP_C::ReceiveDataTimeout(Kernel::HLERequestContext& ctx) {
    pimpl->ReceiveDataTimeout(ctx);
}

void HTTP_C::SetProxyDefault(Kernel::HLERequestContext& ctx) {
    pimpl->SetProxyDefault(ctx);
}

void HTTP_C::AddRequestHeader(Kernel::HLERequestContext& ctx) {
    pimpl->AddRequestHeader(ctx);
}

void HTTP_C::GetResponseHeader(Kernel::HLERequestContext& ctx) {
    pimpl->GetResponseHeader(ctx);
}

void HTTP_C::GetResponseStatusCode(Kernel::HLERequestContext& ctx) {
    pimpl->GetResponseStatusCode(ctx);
}

void HTTP_C::GetResponseStatusCodeTimeout(Kernel::HLERequestContext& ctx) {
    pimpl->GetResponseStatusCodeTimeout(ctx);
}

void HTTP_C::SetSSLOpt(Kernel::HLERequestContext& ctx) {
    pimpl->SetSSLOpt(ctx);
}

void HTTP_C::SetKeepAlive(Kernel::HLERequestContext& ctx) {
    pimpl->SetKeepAlive(ctx);
}

void InstallInterfaces(SM::ServiceManager& service_manager) {
    std::make_shared<HTTP_C>()->InstallAsService(service_manager);
}

} // namespace HTTP
} // namespace Service
