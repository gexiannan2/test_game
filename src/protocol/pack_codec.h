#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zrpc {
class Buffer;
}

struct PackFrame {
  uint8_t flags = 0;
  uint32_t msg_id = 0;
  std::string body;
  uint8_t recv_index = 0;
  // 当 flags & kPackFlagHasIds 时，存放解析出的 ids 数组（如网关转发的 gcid）
  std::vector<uint64_t> ids;
};

// 组包：pack_len(3B LE) + flag(1B) + [ids_len(2B)+ids_arr(N×8B)] + msg_id(4B LE) + body + recv_index(1B)
// 其中 ids 段仅在 flag & kPackFlagHasIds 时存在
// pack_len = 整包字节数（含头尾）
void EncodeFrame(const PackFrame& frame, ::zrpc::Buffer* out);

// 从连接缓冲中解析完整包；可能一次解析出多帧（粘包）
// 返回 false 表示数据不足或格式错误（错误时会清空可读区避免死循环，调用方可断开连接）
bool TryDecodeFrames(::zrpc::Buffer* buf, std::vector<PackFrame>& frames);

uint32_t ReadUint32Le(const char* p);
void WriteUint32Le(uint32_t v, char* p);

uint32_t ReadPackLen24Le(const char* p);
void WritePackLen24Le(uint32_t len, char* p);

