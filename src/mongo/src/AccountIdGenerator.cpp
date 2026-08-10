#include "AccountIdGenerator.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #ifdef __linux__
        #include <netpacket/packet.h>
    #elif defined(__APPLE__)
        #include <net/if_dl.h>
        #include <sys/types.h>
    #endif
#endif

namespace mongo
{
    namespace
    {
        // 获取 16 位 MAC 地址哈希（跨平台）。
        // 同主机永远返回相同值；不同主机有 1/2^16 碰撞概率。
        std::uint16_t GetMacHash16Bit()
        {
            static const std::uint16_t mac_hash = []() -> std::uint16_t
            {
                std::uint32_t hash = 0;

#ifdef _WIN32
                PIP_ADAPTER_INFO adapter_info = nullptr;
                PIP_ADAPTER_INFO adapter = nullptr;
                ULONG out_buf_len = sizeof(IP_ADAPTER_INFO);

                adapter_info = static_cast<PIP_ADAPTER_INFO>(std::malloc(out_buf_len));
                if (adapter_info == nullptr)
                {
                    return 0;
                }

                DWORD ret = GetAdaptersInfo(adapter_info, &out_buf_len);
                if (ret == ERROR_BUFFER_OVERFLOW)
                {
                    std::free(adapter_info);
                    adapter_info = static_cast<PIP_ADAPTER_INFO>(std::malloc(out_buf_len));
                    if (adapter_info == nullptr)
                    {
                        return 0;
                    }
                    ret = GetAdaptersInfo(adapter_info, &out_buf_len);
                }

                if (ret == NO_ERROR)
                {
                    adapter = adapter_info;
                    while (adapter)
                    {
                        // 跳过回环和虚拟网卡
                        if (adapter->Type != MIB_IF_TYPE_LOOPBACK &&
                            adapter->AddressLength == 6)
                        {
                            for (UINT i = 0; i < adapter->AddressLength; ++i)
                            {
                                hash ^= (adapter->Address[i] << ((i % 4) * 8));
                            }
                            break;
                        }
                        adapter = adapter->Next;
                    }
                }
                std::free(adapter_info);
#else
                struct ifaddrs* ifaddr = nullptr;
                if (getifaddrs(&ifaddr) == 0)
                {
                    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
                    {
                        if (ifa->ifa_addr == nullptr)
                        {
                            continue;
                        }

#ifdef __linux__
                        if (ifa->ifa_addr->sa_family == AF_PACKET)
                        {
                            struct sockaddr_ll* sll =
                                reinterpret_cast<struct sockaddr_ll*>(ifa->ifa_addr);
                            if (sll->sll_halen == 6)
                            {
                                unsigned char* mac =
                                    reinterpret_cast<unsigned char*>(sll->sll_addr);
                                bool is_valid = false;
                                for (int i = 0; i < 6; ++i)
                                {
                                    if (mac[i] != 0)
                                    {
                                        is_valid = true;
                                    }
                                }
                                if (is_valid && std::strcmp(ifa->ifa_name, "lo") != 0)
                                {
                                    for (int i = 0; i < 6; ++i)
                                    {
                                        hash ^= (mac[i] << ((i % 4) * 8));
                                    }
                                    break;
                                }
                            }
                        }
#elif defined(__APPLE__)
                        if (ifa->ifa_addr->sa_family == AF_LINK)
                        {
                            struct sockaddr_dl* sdl =
                                reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
                            if (sdl->sdl_alen == 6)
                            {
                                unsigned char* mac =
                                    reinterpret_cast<unsigned char*>(LLADDR(sdl));
                                bool is_valid = false;
                                for (int i = 0; i < 6; ++i)
                                {
                                    if (mac[i] != 0)
                                    {
                                        is_valid = true;
                                    }
                                }
                                if (is_valid && std::strcmp(ifa->ifa_name, "lo0") != 0)
                                {
                                    for (int i = 0; i < 6; ++i)
                                    {
                                        hash ^= (mac[i] << ((i % 4) * 8));
                                    }
                                    break;
                                }
                            }
                        }
#endif
                    }
                    freeifaddrs(ifaddr);
                }
#endif

                // 降级方案：使用主机名 + 随机数
                if (hash == 0)
                {
                    char hostname[256] = {0};
#ifdef _WIN32
                    DWORD size = sizeof(hostname);
                    GetComputerNameA(hostname, &size);
#else
                    gethostname(hostname, sizeof(hostname) - 1);
#endif
                    hash = static_cast<std::uint32_t>(
                        std::hash<std::string>{}(hostname));
                    std::random_device rd;
                    hash ^= rd();
                }

                return static_cast<std::uint16_t>(hash & 0xFFFF);
            }();

            return mac_hash;
        }

        // 获取 8 位毫秒时间戳（约 256 毫秒循环周期）。
        std::uint8_t GetTimestamp8Bit()
        {
            const auto now = std::chrono::system_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            return static_cast<std::uint8_t>(ms & 0xFF);
        }

        // 获取 8 位随机数（线程安全）。
        std::uint8_t GetRandom8Bit()
        {
            static thread_local std::mt19937 rng([]() -> std::uint32_t
            {
                std::random_device rd;
                std::uint64_t seed = rd();
                seed ^= std::chrono::steady_clock::now().time_since_epoch().count();
                seed ^= std::hash<std::thread::id>{}(std::this_thread::get_id());
                return static_cast<std::uint32_t>(seed);
            }());
            static thread_local std::uniform_int_distribution<int> dist(0, 255);
            return static_cast<std::uint8_t>(dist(rng));
        }

        // 生成 32 位母 ID。
        //
        // 格式：[MAC 哈希 16bit][毫秒时间戳 8bit][随机 8bit]
        // 然后 MurmurHash3 风格 finalizer 扰乱，增强雪崩效应。
        //
        // 碰撞概率：
        //   - 同主机：每毫秒 ≤5 个时碰撞率 <4%
        //   - 不同主机：约 1/16,777,216
        std::uint32_t GenMasterId()
        {
            const std::uint16_t mac_hash = GetMacHash16Bit();
            const std::uint8_t timestamp = GetTimestamp8Bit();
            const std::uint8_t random_val = GetRandom8Bit();

            // 拼接：MAC 在高 16 位，时间戳在中 8 位，随机在低 8 位
            std::uint32_t id = (static_cast<std::uint32_t>(mac_hash) << 16) |
                               (static_cast<std::uint32_t>(timestamp) << 8) |
                               random_val;

            // 位扰乱（增强雪崩效应，降低规律性）
            id = (id ^ (id >> 16)) * 0x85EBCA6BU;
            id = id ^ (id >> 13);
            id = id * 0xC2B2AE35U;
            id = id ^ (id >> 16);

            return id;
        }

        // 进程内 ID 管理器单例。
        //
        // 持有 master_id 和 slave_id_idx，所有调用通过 mutex 串行化。
        // master_id 在 slave_id_idx 回绕到 0 时刷新，保证同一周期内 sid 严格单调自增。
        class IdMgr
        {
            public:
                std::mutex mtx;
                std::uint32_t master_id;
                std::uint32_t slave_id_idx;

                IdMgr() : master_id(GenMasterId()) {}
        };

        IdMgr& GetIdMgr()
        {
            static IdMgr mgr;
            return mgr;
        }

        // sid 接近溢出阈值时告警；阈值留足余量，避免 account_id * INDEX_MOD_NUM 溢出 int64。
        // 当 sid >= kWarnThreshold 时，每 100 万次输出一次告警。
        constexpr std::uint32_t kWarnThreshold = 10000000;  // 1 千万

    } // namespace

    std::uint64_t GenerateAccountId()
    {
        auto& mgr = GetIdMgr();

        std::lock_guard<std::mutex> lock(mgr.mtx);

        const auto sid = mgr.slave_id_idx++;

        // sid 溢出回绕到 0 时刷新 master_id
        if (sid == 0)
        {
            mgr.master_id = GenMasterId();
        }

        // 接近溢出阈值时输出告警，提示运维关注
        if (sid >= kWarnThreshold && (sid - kWarnThreshold) % 1000000 == 0)
        {
            std::fprintf(stderr,
                "GenerateAccountId WARNING: slave_id_idx=%u approaching overflow, "
                "consider restarting service to refresh master_id\n",
                sid);
        }

        // 非有序 ID：高 32 位 sid + 低 32 位 master_id
        return (static_cast<std::uint64_t>(sid) << 32) |
               static_cast<std::uint64_t>(mgr.master_id);
    }

} // namespace mongo
