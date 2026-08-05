#include "protocol/pack_codec.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "protocol/pack_flags.h"
#include "zrpc/base/buffer.h"

// PKC_DEBUG=1 时向 stderr 输出 hex dump / 解密过程
#if PKC_DEBUG
#define PKC_DBG(fmt, ...) fprintf(stderr, "[DBG] " fmt "\n", ##__VA_ARGS__)
#define PKC_DBG_RAW(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define PKC_DBG_FLUSH() fflush(stderr)
#else
#define PKC_DBG(fmt, ...) ((void)0)
#define PKC_DBG_RAW(fmt, ...) ((void)0)
#define PKC_DBG_FLUSH() ((void)0)
#endif

// bit6 加解密（摘自 e996_codec/codec_bit6.cpp；ENCRYPT 时处理 msg_id+body）
namespace {
namespace bit6 {

static const int n4CEEF4 = 0x408D4D;
static const uint32_t n4CEEF8 = 0x0C08BA52E;
static const uint16_t w4CEF00 = 0x8D34;
static const int n4CEEFC = 0x408D97;

constexpr static uint8_t DecodeBitMasks[256] = {
    0x2A, 0xE7, 0x18, 0x6F, 0x63, 0x9D, 0x48, 0xEA, 0x39, 0xCD, 0x38, 0xB8, 0xA0, 0xAB, 0xE0, 0x10,
    0x35, 0x99, 0x37, 0x09, 0xC0, 0x69, 0xB2, 0xA4, 0x67, 0x88, 0x50, 0x34, 0x7F, 0xFC, 0x0B, 0xBE,
    0x0C, 0x44, 0x59, 0xB6, 0x5B, 0x9C, 0x65, 0xD6, 0x94, 0xEB, 0xC4, 0x3B, 0x03, 0x3C, 0xC9, 0x3E,
    0x6B, 0x9A, 0xD4, 0xF6, 0xC3, 0x4D, 0x11, 0x24, 0xAA, 0xFF, 0x4A, 0xED, 0x95, 0x93, 0xD9, 0x46,
    0x5F, 0x96, 0x87, 0x30, 0xBA, 0xCA, 0xCB, 0xFA, 0x8A, 0x1A, 0x68, 0x5C, 0xAC, 0x07, 0x40, 0x60,
    0x29, 0x70, 0x57, 0x53, 0x41, 0x12, 0xDE, 0x1D, 0x64, 0x14, 0x97, 0x72, 0xFB, 0x8D, 0x2B, 0x08,
    0xCF, 0xF4, 0x3A, 0x00, 0xC5, 0x91, 0x56, 0xA9, 0x9E, 0x71, 0xBC, 0xA3, 0xAF, 0xA6, 0x55, 0xDA,
    0x79, 0xBB, 0x33, 0xA5, 0x25, 0x15, 0x7D, 0xEE, 0xC1, 0x2C, 0xC7, 0xD0, 0x19, 0xD8, 0x5A, 0xE8,
    0x85, 0xFD, 0x2F, 0x6A, 0x78, 0x45, 0xDB, 0xB5, 0xF5, 0x1E, 0x04, 0x75, 0xB0, 0x7A, 0x20, 0xF2,
    0xDF, 0xD3, 0x83, 0xF3, 0x54, 0x90, 0xA2, 0xC6, 0x0F, 0x80, 0x36, 0x4E, 0xC8, 0x01, 0x82, 0x76,
    0xA1, 0x2E, 0x84, 0x86, 0x0E, 0x47, 0x8F, 0xE1, 0xF9, 0x7C, 0xC2, 0x74, 0xDC, 0x26, 0x22, 0xCE,
    0x2D, 0x4F, 0xBF, 0x0D, 0x73, 0x27, 0x21, 0xB3, 0x98, 0x1F, 0x89, 0xEC, 0xFE, 0x52, 0x0A, 0x8C,
    0x9F, 0xA8, 0xE5, 0xE6, 0x06, 0x8B, 0xCC, 0xF7, 0x5E, 0xE3, 0x7B, 0xD2, 0x05, 0x49, 0x13, 0xE9,
    0x66, 0xB7, 0xAD, 0xB4, 0xF8, 0xA7, 0x1C, 0xF1, 0x02, 0x7E, 0x6E, 0x17, 0x62, 0x4C, 0x77, 0x8E,
    0xDD, 0xF0, 0x43, 0x28, 0x6D, 0x61, 0xB9, 0xD7, 0xBD, 0x3D, 0x9B, 0x92, 0x16, 0xEF, 0x51, 0x23,
    0xE2, 0xB1, 0x81, 0x31, 0x32, 0x58, 0xD1, 0x5D, 0xD5, 0x6C, 0x4B, 0xE4, 0xAE, 0x42, 0x1B, 0x3F
};

constexpr static uint8_t EncodeBitMasks[256] = {
    0x8C, 0x87, 0x0D, 0x85, 0xD4, 0x64, 0x63, 0xE5, 0xBA, 0x7E, 0xB8, 0x68, 0x9D, 0x9F, 0xF5, 0xBC,
    0xA0, 0xE3, 0x3A, 0x22, 0x19, 0x21, 0x39, 0x78, 0xEE, 0x27, 0x36, 0x15, 0x74, 0xC7, 0x97, 0xC9,
    0xCE, 0xE2, 0x7B, 0x4C, 0x98, 0xA1, 0xC2, 0x59, 0x41, 0xC0, 0x1E, 0x2E, 0x95, 0xEB, 0xDE, 0x69,
    0x1D, 0x5B, 0x53, 0xDA, 0xF4, 0x0A, 0x4F, 0xBB, 0xB7, 0x24, 0x33, 0x0F, 0xC8, 0x84, 0x29, 0x89,
    0x3C, 0x1C, 0x08, 0x49, 0xC6, 0xFE, 0xCC, 0x23, 0x3E, 0xE1, 0x4E, 0x8B, 0x13, 0xE7, 0x1A, 0x5D,
    0xCF, 0xB1, 0x47, 0x8F, 0xD8, 0x72, 0x4B, 0x93, 0x6E, 0x73, 0x4D, 0x94, 0xDD, 0x82, 0x14, 0xA7,
    0x03, 0xF9, 0xF1, 0xC5, 0x8D, 0x79, 0x2A, 0xC4, 0xDC, 0x60, 0x5F, 0xD7, 0x62, 0xB5, 0xE9, 0xB3,
    0xB6, 0x12, 0xA8, 0x32, 0xD9, 0xC3, 0x6A, 0x75, 0x4A, 0xA2, 0x0C, 0x26, 0x91, 0x5A, 0xAD, 0x6D,
    0x44, 0x10, 0xB4, 0x46, 0x1B, 0x66, 0x81, 0x20, 0xFD, 0x7F, 0x88, 0x25, 0x9C, 0x71, 0xD3, 0xE6,
    0x80, 0xE4, 0xFA, 0x42, 0x9B, 0x37, 0x01, 0xFC, 0xDB, 0x45, 0x6B, 0xFB, 0x56, 0xF0, 0xAF, 0x9A,
    0xBF, 0xAB, 0xD6, 0xCD, 0x02, 0xF2, 0x7C, 0xAA, 0xB2, 0x92, 0xFF, 0x57, 0x2F, 0x86, 0xA6, 0x7D,
    0x35, 0x17, 0x34, 0xD5, 0x0E, 0x65, 0x09, 0x05, 0x28, 0xCA, 0x48, 0x31, 0x8E, 0x2D, 0xDF, 0x52,
    0xF6, 0x1F, 0xA4, 0x50, 0x76, 0x40, 0x18, 0x04, 0x8A, 0x16, 0x2B, 0xAE, 0x43, 0x3F, 0xD0, 0xCB,
    0x6C, 0x55, 0x54, 0x96, 0x99, 0x30, 0x67, 0x5E, 0x2C, 0xAC, 0xE0, 0x7A, 0xE8, 0x58, 0x90, 0xBE,
    0xA5, 0x6F, 0xB0, 0x70, 0xEC, 0x61, 0x5C, 0x06, 0x3B, 0x77, 0xC1, 0x07, 0xEA, 0xA9, 0xF8, 0x11,
    0xBD, 0xF3, 0x00, 0xED, 0x83, 0xEF, 0x3D, 0xA3, 0x51, 0x9E, 0x38, 0xF7, 0x0B, 0xB9, 0xD2, 0xD1
};

inline uint8_t HiByte(uint16_t in) { return static_cast<uint8_t>((in >> 8) & 0x00FF); }
inline uint8_t LoByte(uint16_t in) { return static_cast<uint8_t>(in & 0x00FF); }
inline uint16_t HiWord(uint32_t in) { return static_cast<uint16_t>((in >> 16) & 0xFFFF); }
inline uint16_t LoWord(uint32_t in) { return static_cast<uint16_t>(in & 0xFFFF); }

// 加密: 输入明文 in[0..in_len), 输出密文 out[0..out_len), 返回密文长度
int encrypt(const char* in, size_t in_len, char* out, size_t out_len) {
    int nRestCount = 0;
    int nDestPos = 0;
    uint8_t btMade;
    uint8_t btCh;
    uint8_t btRest = 0;

    for (int i = 0; i <= (int)in_len - 1; i++) {
        if (nDestPos >= (int)out_len) break;
        btCh = in[i];
        btCh = (EncodeBitMasks[btCh] ^ n4CEEFC) ^ n4CEEF4;
        btCh = btCh ^ (HiByte(LoWord(n4CEEF8)) + LoByte(LoWord(n4CEEF8)));
        btMade = uint8_t((btRest | (btCh >> (2 + nRestCount))) & 0x3F);
        btRest = uint8_t(((btCh << (8 - (2 + nRestCount))) >> 2) & 0x3F);
        nRestCount += 2;
        if (nRestCount < 6) {
            out[nDestPos] = uint8_t(btMade + 0x3C);
            nDestPos++;
        } else {
            if (nDestPos < (int)out_len - 1) {
                out[nDestPos] = uint8_t(btMade + 0x3C);
                out[nDestPos + 1] = uint8_t(btRest + 0x3C);
                nDestPos += 2;
            } else {
                out[nDestPos] = uint8_t(btMade + 0x3C);
                nDestPos++;
            }
            nRestCount = 0;
            btRest = 0;
        }
    }
    if (nRestCount > 0) {
        out[nDestPos] = uint8_t(btRest + 0x3C);
        nDestPos++;
    }
    return nDestPos;
}

// 解密: 输入密文 in[0..in_len), 输出明文 out[0..out_len), 返回明文长度
int decrypt(const char* in, size_t in_len, char* out, size_t out_len) {
    uint8_t Masks[] = { 0x00, 0x00, 0xFC, 0xF8, 0xF0, 0xE0, 0xC0 };
    int nBitPos = 2;
    int nMadeBit = 0;
    int nDestPos = 0;
    uint8_t btCh;
    uint8_t btTmp = 0;
    uint8_t btByte;

    for (int i = 0; i <= (int)in_len - 1; i++) {
        if (int(in[i]) - 0x3C >= 0) {
            btCh = uint8_t(in[i]) - 0x3C;
        } else {
            nDestPos = 0;
            break;
        }
        if (nDestPos >= (int)out_len) break;
        if ((nMadeBit + 6) >= 8) {
            btByte = uint8_t(btTmp | ((btCh & 0x3F) >> (6 - nBitPos)));
            btByte = btByte ^ (HiByte(LoWord(n4CEEF8)) + LoByte(LoWord(n4CEEF8)));
            btByte = btByte ^ LoByte(LoWord(n4CEEF4));
            btByte = DecodeBitMasks[btByte] ^ LoByte(w4CEF00);
            out[nDestPos] = btByte;
            nDestPos++;
            nMadeBit = 0;
            if (nBitPos < 6) {
                nBitPos += 2;
            } else {
                nBitPos = 2;
                continue;
            }
        }
        btTmp = uint8_t(uint8_t(btCh << nBitPos) & Masks[nBitPos]);
        nMadeBit += (8 - nBitPos);
    }
    return nDestPos;
}

}  // namespace bit6
}  // namespace

namespace {

constexpr uint32_t kPackLenMax = 0x00ffffffu;
// 解密缓冲区上限（与 svc_gate g_net_cfg.max_msg_pack_size 一致）
constexpr size_t kDecryptBufSize = 1024 * 1024;

}  // namespace

uint32_t ReadUint32Le(const char* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

void WriteUint32Le(uint32_t v, char* p) {
  std::memcpy(p, &v, sizeof(v));
}

uint32_t ReadPackLen24Le(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16);
}

void WritePackLen24Le(uint32_t len, char* p) {
  p[0] = static_cast<char>(len & 0xff);
  p[1] = static_cast<char>((len >> 8) & 0xff);
  p[2] = static_cast<char>((len >> 16) & 0xff);
}

void EncodeFrame(const PackFrame& frame, zrpc::Buffer* out) {
  if (!out) {
    return;
  }

  uint8_t flag = frame.flags;
  const size_t msg_len = frame.body.size();

  size_t plain_size = sizeof(uint32_t) + msg_len;
  std::vector<char> plain(plain_size);
  WriteUint32Le(frame.msg_id, plain.data());
  if (msg_len > 0) {
    std::memcpy(plain.data() + sizeof(uint32_t), frame.body.data(), msg_len);
  }

  std::vector<char> cipher;
  size_t payload_size = plain_size;  // 最终写入 buf 的字节数
  if (flag & kPackFlagEncrypt) {
    cipher.resize(plain_size * 3 + 64);
    int newlen = bit6::encrypt(plain.data(), plain_size,
                               cipher.data(), cipher.size());
    if (newlen >= 0) {
      cipher.resize(newlen);
      payload_size = static_cast<size_t>(newlen);
    } else {
      // 加密失败降级为明文
      flag &= ~kPackFlagEncrypt;
      cipher = plain;
      payload_size = plain_size;
    }
  } else {
    cipher = plain;
  }

  uint32_t pack_len = static_cast<uint32_t>(kPackHeaderSize);
  if (flag & kPackFlagHasIds) {
    if (frame.ids.size() > 0xffffu) {
      PKC_DBG("EncodeFrame: ids.size()=%zu exceeds uint16_t", frame.ids.size());
      PKC_DBG_FLUSH();
      return;
    }
    pack_len += sizeof(uint16_t) + frame.ids.size() * sizeof(uint64_t);
  }
  // 用 size_t 累加防截断，再校验 24-bit 上限
  const size_t total_size = static_cast<size_t>(pack_len) + payload_size + kPackTailSize;
  if (total_size > kPackLenMax) {
    PKC_DBG("EncodeFrame: pack too large size=%zu max=%u", total_size, kPackLenMax);
    PKC_DBG_FLUSH();
    return;
  }
  pack_len = static_cast<uint32_t>(total_size);

  char header[kPackHeaderSize];
  WritePackLen24Le(pack_len, header);
  header[3] = static_cast<char>(flag);
  out->Append(header, static_cast<int32_t>(sizeof(header)));

  if (flag & kPackFlagHasIds) {
    uint16_t ids_len = static_cast<uint16_t>(frame.ids.size());
    out->Append(reinterpret_cast<const char*>(&ids_len),
                static_cast<int32_t>(sizeof(ids_len)));
    for (uint64_t id : frame.ids) {
      out->Append(reinterpret_cast<const char*>(&id),
                  static_cast<int32_t>(sizeof(id)));
    }
  }

  out->Append(cipher.data(), static_cast<int32_t>(payload_size));

  out->AppendInt8(static_cast<int8_t>(frame.recv_index));
}

// 解包逻辑对照 svc_gate net_decoder.cpp
bool TryDecodeFrames(zrpc::Buffer* buf, std::vector<PackFrame>& frames) {
  frames.clear();
  if (!buf) {
    return false;
  }

  while (buf->ReadableBytes() >= static_cast<int32_t>(kPackHeaderSize)) {
    const char* base = buf->Peek();

    // hex dump（仅 PKC_DEBUG）
    PKC_DBG("TryDecode: readable=%d", buf->ReadableBytes());
    if constexpr (PKC_DEBUG) {
      int dump_len = buf->ReadableBytes();
      if (dump_len > 24) dump_len = 24;
      fprintf(stderr, "[DBG] hex(%d): ", dump_len);
      for (int i = 0; i < dump_len; ++i) {
        fprintf(stderr, "%02X ", static_cast<unsigned char>(base[i]));
      }
      fprintf(stderr, "\n");
      fflush(stderr);
    }

    const uint32_t pack_len = ReadPackLen24Le(base);
    const uint8_t flag = static_cast<uint8_t>(base[3]);

    if (pack_len < kPackMinSize || pack_len > kPackLenMax) {
      PKC_DBG("invalid pack_len=%u", pack_len);
      PKC_DBG_FLUSH();
      buf->RetrieveAll();
      return false;
    }
    if (buf->ReadableBytes() < static_cast<int32_t>(pack_len)) {
      break;
    }

    PKC_DBG("pack_len=%u flag=0x%02X", pack_len, flag);
    PKC_DBG_FLUSH();

    const uint8_t recv_index =
        static_cast<uint8_t>(base[pack_len - kPackTailSize]);

    int msg_len = static_cast<int>(pack_len) -
                  static_cast<int>(kPackHeaderSize + kPackTailSize);

    // cursor 指向 head 之后
    const char* cursor = base + kPackHeaderSize;

    PackFrame frame;
    frame.flags = flag;
    frame.recv_index = recv_index;

    if (flag & kPackFlagHasIds) {
      if (msg_len < static_cast<int>(sizeof(uint16_t))) {
        buf->RetrieveAll();
        return false;
      }
      uint16_t ids_len = 0;
      std::memcpy(&ids_len, cursor, sizeof(uint16_t));
      cursor += sizeof(uint16_t);
      msg_len -= sizeof(uint16_t);

      const int ids_size = ids_len * static_cast<int>(sizeof(uint64_t));
      if (ids_size > msg_len) {
        buf->RetrieveAll();
        return false;
      }
      frame.ids.resize(ids_len);
      for (uint16_t i = 0; i < ids_len; ++i) {
        std::memcpy(&frame.ids[i], cursor, sizeof(uint64_t));
        cursor += sizeof(uint64_t);
      }
      msg_len -= ids_size;
    }

    const char* payload = cursor;
    size_t payload_len = static_cast<size_t>(msg_len);
    std::vector<char> decrypt_buf_vec;  // 仅在需要时分配

    if (flag & kPackFlagZips) {
      // Snappy 未接入：拒绝 ZIPS 帧，避免把压缩载荷当明文解密/解析
      PKC_DBG("ZIPS unsupported, reject frame (cipher_len=%d)", msg_len);
      PKC_DBG_FLUSH();
      buf->RetrieveAll();
      return false;
    }

    if (flag & kPackFlagEncrypt) {
      PKC_DBG("decrypting: cipher_len=%d", msg_len);
      PKC_DBG_FLUSH();

      static thread_local std::vector<char> decrypt_buf;
      if (decrypt_buf.size() < kDecryptBufSize) {
        decrypt_buf.resize(kDecryptBufSize);
      }
      int decrypt_size = bit6::decrypt(payload, payload_len,
                                       decrypt_buf.data(), kDecryptBufSize);
      if (decrypt_size < static_cast<int>(sizeof(uint32_t))) {
        PKC_DBG("decrypt failed: size=%d", decrypt_size);
        PKC_DBG_FLUSH();
        buf->RetrieveAll();
        return false;
      }

      if constexpr (PKC_DEBUG) {
        fprintf(stderr, "[DBG] decrypted %d bytes, hex(8): ", decrypt_size);
        for (int i = 0; i < decrypt_size && i < 8; ++i) {
          fprintf(stderr, "%02X ", static_cast<unsigned char>(decrypt_buf[i]));
        }
        fprintf(stderr, "\n");
        fflush(stderr);
      }

      payload = decrypt_buf.data();
      payload_len = static_cast<size_t>(decrypt_size);
    }

    if (payload_len < kPackMsgIdSize) {
      buf->RetrieveAll();
      return false;
    }
    frame.msg_id = ReadUint32Le(payload);
    size_t body_len = payload_len - kPackMsgIdSize;
    if (body_len > 0) {
      frame.body.assign(payload + kPackMsgIdSize, body_len);
    }

    PKC_DBG("parsed: msg_id=%u (0x%08X) body_len=%zu recv_idx=%u ids=%zu",
            frame.msg_id, frame.msg_id, frame.body.size(), frame.recv_index, frame.ids.size());
    PKC_DBG_FLUSH();

    buf->Retrieve(static_cast<int32_t>(pack_len));

    if ((flag & kPackFlagMerged) != 0 && !frame.body.empty()) {
      zrpc::Buffer inner;
      inner.Append(frame.body.data(),
                   static_cast<int32_t>(frame.body.size()));
      std::vector<PackFrame> nested;
      if (!TryDecodeFrames(&inner, nested)) {
        return false;
      }
      frames.insert(frames.end(), nested.begin(), nested.end());
    } else {
      frames.push_back(std::move(frame));
    }
  }
  return true;
}

