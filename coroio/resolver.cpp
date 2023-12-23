#include "resolver.hpp"
#include "socket.hpp"
#include "promises.hpp"
#ifdef __linux__
#include "uring.hpp"
#endif
#include <string_view>
#include <utility>
#include <fstream>

namespace NNet {

namespace {

// Based on https://w3.cs.jmu.edu/kirkpams/OpenCSF/Books/csf/html/UDPSockets.html

struct TDnsHeader {
    uint16_t xid = 0;      /* Randomly chosen identifier */
    uint16_t flags = 0;    /* Bit-mask to indicate request/response */
    uint16_t qdcount = 0;  /* Number of questions */
    uint16_t ancount = 0;  /* Number of answers */
    uint16_t nscount = 0;  /* Number of authority records */
    uint16_t arcount = 0;  /* Number of additional records */
} __attribute__((__packed__));

struct TDnsQuestion {
    char* name;        /* Pointer to the domain name in memory */
    uint16_t dnstype;  /* The QTYPE (1 = A) */
    uint16_t dnsclass; /* The QCLASS (1 = IN) */
} __attribute__((__packed__));

struct TDnsRecordA {
    uint16_t type;
    uint16_t clazz;
    uint32_t ttl;
    uint16_t length;
    char addr[0];
} __attribute__((packed));

static_assert(sizeof(TDnsRecordA) == 10);

void CreatePacket(const std::string& name, EDNSType type, char* packet, int* size, uint16_t xid)
{
    TDnsHeader header = {
        .xid = htons(xid),
        .flags = htons(0x0100), /* Q=0, RD=1 */
        .qdcount = htons(1) /* Sending 1 question */
    };

    std::string query; query.resize(name.size() + 2);
    TDnsQuestion question = {
        .name = &query[0],
        .dnstype = htons (static_cast<uint16_t>(type)),  /* QTYPE 1=A */
        .dnsclass = htons (1), /* QCLASS 1=IN */
    };

    memcpy (question.name + 1, &name[0], name.size());
    uint8_t* prev = (uint8_t*) question.name;
    uint8_t count = 0; /* Used to count the bytes in a field */

    /* Traverse through the name, looking for the . locations */
    for (size_t i = 0; i < name.size(); i++)
    {
        /* A . indicates the end of a field */
        if (name[i] == '.') {
            /* Copy the length to the byte before this field, then
            update prev to the location of the . */
            *prev = count;
            prev = (uint8_t*)question.name + i + 1;
            count = 0;
        }
        else {
            count++;
        }
    }
    *prev = count;

    size_t packetlen = sizeof (header) + name.size() + 2 +
        sizeof (question.dnstype) + sizeof (question.dnsclass);
    assert(packetlen <= 4096);
    *size = packetlen;

    uint8_t *p = (uint8_t *)packet;

    /* Copy the header first */
    memcpy (p, &header, sizeof (header));
    p += sizeof (header);

    /* Copy the question name, QTYPE, and QCLASS fields */
    memcpy(p, question.name, name.size() + 1);
    p += name.size() + 2; /* includes 0 octet for end */
    memcpy(p, &question.dnstype, sizeof (question.dnstype));
    p += sizeof (question.dnstype);
    memcpy(p, &question.dnsclass, sizeof (question.dnsclass));
}

void ParsePacket(uint16_t* xid, std::vector<TAddress>& addresses, char* buf, ssize_t size) {
    TDnsHeader* header = (TDnsHeader*)(&buf[0]);
    *xid = ntohs(header->xid);
    if ((ntohs (header->flags) & 0xf) != 0) {
        throw std::runtime_error("Resolver Error");
    }
    uint8_t* startOfName = (uint8_t*)(&buf[0] + sizeof (TDnsHeader));
    uint8_t fragmentSize = 0;
    uint8_t* p = startOfName; size -= p - (uint8_t*)buf; if (size <= 0) { throw std::runtime_error("Not enough data"); }

    while (*p != 0)
    {
        /* Restore the dot in the name and advance to next length */
        fragmentSize = *p + 1;
        *p = '.';
        p += fragmentSize; size -= fragmentSize; if (size <= 0) { throw std::runtime_error("Not enough data"); }
    }

    addresses.reserve(header->ancount);
    p += 5; size -= 5; if (size <= 0) { throw std::runtime_error("Not enough data"); }
    for (int i = 0; i < ntohs (header->ancount); i++)
    {
        uint16_t* compression = (uint16_t*)p; p += 2; size -= 2; if (size <= 0) { throw std::runtime_error("Not enough data"); }
        if (! ((ntohs(*compression) & 0xC000) == 0xC000)) {
            // skip full name
            while (*p) {
                p++; size--; if (size <= 0) { throw std::runtime_error("Not enough data"); }
            }
            p++; size--; if (size <= 0) { throw std::runtime_error("Not enough data"); }
        }

        TDnsRecordA* record = (TDnsRecordA*)p;

        auto addrLen = ntohs(record->length);
        if (addrLen == 4) {
            sockaddr_in addr = {
                .sin_port = 0,
                .sin_addr = *(in_addr*)record->addr,
            };
            addresses.emplace_back(TAddress{addr});
        } else if (addrLen == 16) {
            sockaddr_in6 addr = {
                .sin6_port = 0,
                .sin6_addr = *(in6_addr*)record->addr
            };
            addresses.emplace_back(TAddress{addr});
        }
        p += sizeof(TDnsRecordA);
        p += addrLen;
        size -= sizeof(TDnsRecordA); if (size < 0) { throw std::runtime_error("Not enough data"); }
    }
}

} // namespace

TResolvConf::TResolvConf(const std::string& fn)
{
    std::ifstream input(fn);
    Load(input);
}

TResolvConf::TResolvConf(std::istream& input) {
    Load(input);
}

void TResolvConf::Load(std::istream& input) {
    for (std::string line; getline(input, line);) {
        std::vector<std::string> tokens;
        const char* sep = " ";
        for (char* tok = strtok(line.data(), sep); tok; tok = strtok(nullptr, sep)) {
            tokens.push_back(tok);
        }
        if (tokens.size() == 2 && tokens[0] == "nameserver") {
            auto addr = TAddress{tokens[1], 53};
            Nameservers.emplace_back(std::move(addr));
        }
    }

    if (Nameservers.empty()) {
        Nameservers.emplace_back(TAddress{"127.0.0.1", 53});
    }
}

template<typename TPoller>
TResolver<TPoller>::TResolver(TPoller& poller)
    : TResolver(TResolvConf(), poller)
{ }

template<typename TPoller>
TResolver<TPoller>::TResolver(const TResolvConf& conf, TPoller& poller)
    : TResolver(conf.Nameservers[0], poller)
{ }

template<typename TPoller>
TResolver<TPoller>::TResolver(TAddress dnsAddr, TPoller& poller)
    : Socket(std::move(dnsAddr), poller, SOCK_DGRAM)
    , Poller(poller)
{
    // Start tasks after fields initialization
    Sender = SenderTask();
    Receiver = ReceiverTask();
    Timeouts = TimeoutsTask();
}

template<typename TPoller>
TResolver<TPoller>::~TResolver()
{
    Sender.destroy();
    Receiver.destroy();
    Timeouts.destroy();
}

template<typename TPoller>
TVoidSuspendedTask TResolver<TPoller>::SenderTask() {
    co_await Socket.Connect();
    char buf[512];
    while (true) {
        while (AddResolveQueue.empty()) {
            SenderSuspended = co_await SelfId();
            co_await std::suspend_always{};
        }
        SenderSuspended = {};
        auto req = AddResolveQueue.front(); AddResolveQueue.pop();
        int len;
        memset(buf, 0, sizeof(buf));
        Inflight[Xid] = req;
        CreatePacket(req.Name, req.Type, buf, &len, Xid);
        Xid = 1 + (Xid + 1) % 65535;
        auto size = co_await Socket.WriteSome(buf, len);
        assert(size == len);
    }
    co_return;
}

template<typename TPoller>
TVoidSuspendedTask TResolver<TPoller>::TimeoutsTask() {
    while (true) {
        TTime now = TClock::now();
        while (!TimeoutsQueue.empty() && TimeoutsQueue.front().first <= now) {
            auto req = std::move(TimeoutsQueue.front().second);
            ResumeWaiters({.Exception = std::make_exception_ptr(std::runtime_error("Timeout"))}, req);
            TimeoutsQueue.pop();
        }
        co_await Poller.Sleep(now + std::chrono::milliseconds(100));
    }
}

template<typename TPoller>
void TResolver<TPoller>::ResumeWaiters(TResolveResult&& result, const TResolveRequest& req) {
    auto maybeWaiting = WaitingAddrs.find(req);
    if (maybeWaiting != WaitingAddrs.end()) {
        Results[req] = std::move(result);
        auto handles = std::move(maybeWaiting->second);
        WaitingAddrs.erase(maybeWaiting);
        for (auto h : handles) {
            h.resume();
        }
    }
}

template<typename TPoller>
TVoidSuspendedTask TResolver<TPoller>::ReceiverTask() {
    char buf[512];
    while (true) {
        auto size = co_await Socket.ReadSome(buf, sizeof(buf));
        if (size < 0) {
            continue;
        }
        if (size < sizeof(TDnsHeader)) {
            continue;
        }

        std::vector<TAddress> addresses;
        std::exception_ptr exception;
        uint16_t xid;
        try {
            ParsePacket(&xid, addresses, buf, size);
        } catch (const std::exception& ex) {
            exception = std::current_exception();
        }

        ResumeWaiters(std::move(TResolveResult {
            .Addresses = std::move(addresses),
            .Exception = exception
        }), Inflight[xid]);
    }
    co_return;
}

template<typename TPoller>
void TResolver<TPoller>::ResumeSender() {
    if (SenderSuspended) {
        SenderSuspended.resume();
    }
}

template<typename TPoller>
TValueTask<std::vector<TAddress>> TResolver<TPoller>::Resolve(const std::string& hostname, EDNSType type) {
    auto handle = co_await SelfId();

    TResolveRequest req = {.Name = hostname, .Type = type};

    if (!WaitingAddrs.contains(req)) {
        Results[req].Retries = 5;
        AddResolveQueue.emplace(req);
        TimeoutsQueue.emplace(std::make_pair(TClock::now() + std::chrono::milliseconds(2000), req));
    }
    WaitingAddrs[req].emplace_back(handle);
    ResumeSender();
    co_await std::suspend_always{};
    auto& result = Results[req];
    if (result.Exception) {
        std::rethrow_exception(result.Exception);
    }
    co_return result.Addresses;
}

template class TResolver<TPollerBase>;
#ifdef __linux__
template class TResolver<TUring>;
#endif

} // namespace NNet
