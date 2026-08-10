// 高度图模块 — 从 .hgt 文件加载，提供双线性插值和分层高度查询。

#include "navigation/nav_system.h"

#include <cstdint>
#include <climits>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>

namespace game::navigation
{

    struct HeightMap
    {
        float origin_x = 0.0f;
        float origin_z = 0.0f;
        int   width = 0;
        int   depth = 0;
        float cell_size = 0.1f;
        int   data_size = 0;
        float max_x = 0.0f;
        float max_z = 0.0f;
        float max_height = 0.0f;   // 地形最高点(米)，加载时遍历 heights/extra_vals 计算

        std::vector<int16_t>  heights;
        bool   has_multi = false;
        bool   is_cs_format = false;   // true=C#变长多层格式, false=C++固定格式(type==3)
        std::vector<uint16_t> extra_idx;
        std::vector<int16_t>  extra_vals;
        std::vector<uint8_t>  extra_cnt;

        static constexpr float  SENTINEL_NO_HIT_F = -9999.0f;
        static constexpr int16_t SENTINEL_NO_HIT_I = INT16_MIN;
        static constexpr float  HEIGHT_SCALE = 100.0f;

        static inline float i2f(int16_t v)
        {
            return static_cast<float>(v) / HEIGHT_SCALE;
        }

        bool load(const char* file_path)
        {
            clear();

            if (!file_path || !file_path[0])
            {
                return false;
            }

            FILE* fp = fopen(file_path, "rb");

            if (!fp)
            {
                fprintf(stderr, "[HeightMap] Failed to open: %s\n", file_path);
                return false;
            }

            uint8_t first_byte = 0;

            if (fread(&first_byte, 1, 1, fp) != 1)
            {
                fprintf(stderr, "[HeightMap] File too small: %s\n", file_path);
                fclose(fp);
                return false;
            }

            bool is_new_format = (first_byte == 1 || first_byte == 2 || first_byte == 3);
            int  data_type = is_new_format ? (int)first_byte : 0;

            if (is_new_format)
            {
                fseek(fp, 8, SEEK_CUR);
            }
            else
            {
                fseek(fp, 0, SEEK_SET);
            }

            if (fread(&origin_x, sizeof(float), 1, fp) != 1 ||
                fread(&origin_z, sizeof(float), 1, fp) != 1 ||
                fread(&width, sizeof(int), 1, fp) != 1 ||
                fread(&depth, sizeof(int), 1, fp) != 1 ||
                fread(&cell_size, sizeof(float), 1, fp) != 1 ||
                fread(&data_size, sizeof(int), 1, fp) != 1)
            {
                fprintf(stderr, "[HeightMap] Failed to read header: %s\n", file_path);
                fclose(fp);
                clear();
                return false;
            }

            if (width <= 0 || depth <= 0 || cell_size <= 0.0f)
            {
                fprintf(stderr, "[HeightMap] Invalid header: w=%d d=%d cell=%f\n", width, depth, cell_size);
                fclose(fp);
                clear();
                return false;
            }

            size_t total_samples = (size_t)width * depth;
            int    elem_size = (data_type == 0) ? (int)sizeof(float)
                : (data_type == 1) ? (int)sizeof(int16_t)
                : (data_type == 2) ? (int)sizeof(int32_t)
                : 0;

            // 检测 C# 新多层格式: data_type==1/2 且 data_size 远大于纯数据大小
            bool is_cs_multi = false;

            if (data_type == 1 || data_type == 2)
            {
                int expected_simple = (int)(total_samples * elem_size);

                if (data_size > expected_simple)
                {
                    is_cs_multi = true;  // 有 IndexTable + 变长数据，是C#多层格式
                }
            }

            if (!is_cs_multi && data_type != 3)
            {
                int expected = (int)(total_samples * elem_size);

                if (data_size != expected)
                {
                    fprintf(stderr, "[HeightMap] Header mismatch: dataSize=%d, expected=%d (fmt=%s, w=%d d=%d)\n",
                        data_size, expected,
                        data_type == 0 ? "float" : data_type == 1 ? "int16" : "int32",
                        width, depth);
                    fclose(fp);
                    clear();
                    return false;
                }
            }

            max_x = origin_x + (width - 1) * cell_size;
            max_z = origin_z + (depth - 1) * cell_size;

            heights.resize(total_samples);

            // ── C# 新多层变长格式 ──
            if (is_cs_multi)
            {
                int elem_sz = elem_size;  // 2 (short) 或 4 (int)

                // 1) 读 IndexTable: total_samples 个 uint32
                std::vector<uint32_t> indexTable(total_samples);

                if (fread(indexTable.data(), sizeof(uint32_t), total_samples, fp) != total_samples)
                {
                    fprintf(stderr, "[HeightMap] Read IndexTable fail\n");
                    fclose(fp);
                    clear();
                    return false;
                }

                // 2) 一次Disk IO读取剩余所有CellData（避免反复seek）
                std::vector<uint8_t> cellBuf(data_size - (int)(total_samples * sizeof(uint32_t)));

                if (fread(cellBuf.data(), 1, cellBuf.size(), fp) != cellBuf.size())
                {
                    fprintf(stderr, "[HeightMap] Read CellData fail\n");
                    fclose(fp);
                    clear();
                    return false;
                }

                    // 3) 逐cell解析
                    has_multi = true;
                    is_cs_format = true;
                    extra_idx.assign(total_samples, 0xFFFF);
                extra_cnt.assign(total_samples, 0);
                extra_vals.reserve(total_samples);

                static const int CS_HEADER_SIZE = 33;  // 1B dataType + 8B minH/maxH + 24B header

                for (size_t cell = 0; cell < total_samples; ++cell)
                {
                    uint32_t offset = indexTable[cell];

                    if (offset == 0)
                    {
                        heights[cell] = SENTINEL_NO_HIT_I;
                        continue;
                    }

                    int dataOff = (int)(offset - CS_HEADER_SIZE);           // 在 dataBytes(C#) 中的偏移
                    int cellBufOff = dataOff - (int)(total_samples * sizeof(uint32_t)); // 在 cellBuf 中的偏移

                    if (cellBufOff < 0 || (size_t)cellBufOff >= cellBuf.size())
                    {
                        heights[cell] = SENTINEL_NO_HIT_I;
                        continue;
                    }

                    uint8_t lc = cellBuf[cellBufOff];                       // 层数

                    if (lc == 0)
                    {
                        heights[cell] = SENTINEL_NO_HIT_I;
                        continue;
                    }

                    // 取最高层（最后一项）写回 heights
                    int topOff = cellBufOff + 1 + (lc - 1) * elem_sz;

                    if (elem_sz == 4)
                    {
                        int32_t h;
                        memcpy(&h, &cellBuf[topOff], sizeof(int32_t));
                        heights[cell] = (h == INT_MIN) ? SENTINEL_NO_HIT_I : static_cast<int16_t>(h);
                    }
                    else
                    {
                        int16_t h;
                        memcpy(&h, &cellBuf[topOff], sizeof(int16_t));
                        heights[cell] = h;
                    }

                    // 保存下层到 extra_vals
                    if (lc > 1)
                    {
                        extra_idx[cell] = (uint16_t)extra_vals.size();
                        extra_cnt[cell] = (uint8_t)(lc - 1);

                        for (uint8_t k = 0; k < lc - 1; ++k)
                        {
                            int loOff = cellBufOff + 1 + k * elem_sz;

                            if (elem_sz == 4)
                            {
                                int32_t ih;
                                memcpy(&ih, &cellBuf[loOff], sizeof(int32_t));
                                extra_vals.push_back(static_cast<int16_t>(ih));
                            }
                            else
                            {
                                int16_t ih;
                                memcpy(&ih, &cellBuf[loOff], sizeof(int16_t));
                                extra_vals.push_back(ih);
                            }
                        }
                    }
                }
            }
            // ── C++旧多层格式 (data_type==3) ──
            else if (data_type == 3)
            {
                has_multi = true;
                extra_idx.assign(total_samples, 0xFFFF);
                extra_cnt.assign(total_samples, 0);
                uint16_t lc;
                extra_vals.reserve(total_samples);

                for (size_t cell = 0; cell < total_samples; ++cell)
                {
                    if (fread(&lc, sizeof(uint16_t), 1, fp) != 1)
                    {
                        fprintf(stderr, "[HeightMap] Read layer_count fail (multi) at cell %zu\n", cell);
                        fclose(fp);
                        clear();
                        return false;
                    }

                    if (lc == 0)
                    {
                        heights[cell] = SENTINEL_NO_HIT_I;
                        continue;
                    }

                    std::vector<int16_t> layers(lc);

                    if (fread(layers.data(), sizeof(int16_t), lc, fp) != lc)
                    {
                        fprintf(stderr, "[HeightMap] Read layers fail (multi) at cell %zu\n", cell);
                        fclose(fp);
                        clear();
                        return false;
                    }

                    heights[cell] = layers[0];

                    if (lc > 1)
                    {
                        extra_idx[cell] = (uint16_t)extra_vals.size();
                        extra_cnt[cell] = (uint8_t)(lc - 1);

                        for (size_t k = 1; k < lc; ++k)
                        {
                            extra_vals.push_back(layers[k]);
                        }
                    }
                }
            }
            // ── 旧 int16 单层 ──
            else if (data_type == 1)
            {
                if (fread(heights.data(), sizeof(int16_t), total_samples, fp) != total_samples)
                {
                    fprintf(stderr, "[HeightMap] Read incomplete (int16): %s\n", file_path);
                    fclose(fp);
                    clear();
                    return false;
                }
            }
            // ── 旧 int32 单层 ──
            else if (data_type == 2)
            {
                constexpr size_t BUF32 = 32768;
                std::vector<int32_t> buf(BUF32);
                size_t remaining = total_samples, base = 0;

                while (remaining > 0)
                {
                    size_t n = remaining < BUF32 ? remaining : BUF32;

                    if (fread(buf.data(), sizeof(int32_t), n, fp) != n)
                    {
                        fprintf(stderr, "[HeightMap] Read incomplete (int32): %s\n", file_path);
                        fclose(fp);
                        clear();
                        return false;
                    }

                    for (size_t i = 0; i < n; ++i)
                    {
                        heights[base + i] = (int16_t)buf[i];
                    }

                    base += n;
                    remaining -= n;
                }
            }
            // ── 旧 float 单层 ──
            else
            {
                constexpr size_t BUF_F = 65536;
                std::vector<float> buf(BUF_F);
                size_t remaining = total_samples, base = 0;

                while (remaining > 0)
                {
                    size_t n = remaining < BUF_F ? remaining : BUF_F;

                    if (fread(buf.data(), sizeof(float), n, fp) != n)
                    {
                        fprintf(stderr, "[HeightMap] Read incomplete (float): %s\n", file_path);
                        fclose(fp);
                        clear();
                        return false;
                    }

                    for (size_t i = 0; i < n; ++i)
                    {
                        float fv = buf[i];
                        heights[base + i] = (fv == SENTINEL_NO_HIT_F)
                            ? SENTINEL_NO_HIT_I
                            : static_cast<int16_t>(fv * HEIGHT_SCALE + 0.5f);
                    }

                    base += n;
                    remaining -= n;
                }
            }

        fclose(fp);

        // 计算地形最高点(米)：遍历主层 + 多层额外高度，跳过哨兵值
        max_height = 0.0f;
        for (size_t i = 0; i < heights.size(); ++i)
        {
            if (heights[i] == SENTINEL_NO_HIT_I)
            {
                continue;
            }
            float h = i2f(heights[i]);
            if (h > max_height)
            {
                max_height = h;
            }
        }
        for (size_t i = 0; i < extra_vals.size(); ++i)
        {
            if (extra_vals[i] == SENTINEL_NO_HIT_I)
            {
                continue;
            }
            float h = i2f(extra_vals[i]);
            if (h > max_height)
            {
                max_height = h;
            }
        }

        size_t struct_size = sizeof(*this);
        size_t vector_meta = sizeof(heights);
        size_t heap_bytes = heights.size() * sizeof(int16_t);
        size_t extra_bytes = extra_vals.size() * sizeof(int16_t)
            + extra_idx.size() * sizeof(uint16_t)
            + extra_cnt.size() * sizeof(uint8_t);
        size_t total_bytes = struct_size + heap_bytes + extra_bytes;

        const char* fmt_name = is_cs_multi
            ? (data_type == 1 ? "multi-short(CS)" : "multi-int(CS)")
            : (data_type == 0) ? "float(old)"
            : (data_type == 1) ? "int16(old)"
            : (data_type == 2) ? "int32(old)"
            : "multi(old)";

        fprintf(stdout,
            "\n========== [HeightMap] Memory Stats ==========\n"
            "  Format:      %s\n"
            "  struct sizeof(HeightMap):   %zu B\n"
            "  std::vector meta (inline):  %zu B\n"
            "  heap data (int16_t):        %zu B (%.2f MB)\n"
            "  extra multi-layer:          %zu B (%.2f MB)\n"
            "  heap capacity:              %zu B (%.2f MB)\n"
            "  -------------------------------------\n"
            "  TOTAL g_heightMap:          %zu B (%.2f MB)\n"
            "  Grid:  %d x %d  step=%.2f  precision=1cm\n"
            "  Range: (%.1f,%.1f)->(%.1f,%.1f)\n"
            "=============================================\n"
            "\n",
            fmt_name, struct_size, vector_meta,
            heap_bytes, (float)heap_bytes / (1024.0f * 1024.0f),
            extra_bytes, (float)extra_bytes / (1024.0f * 1024.0f),
            heights.capacity() * sizeof(int16_t),
            (float)(heights.capacity() * sizeof(int16_t)) / (1024.0f * 1024.0f),
            total_bytes, (float)total_bytes / (1024.0f * 1024.0f),
            width, depth, cell_size, origin_x, origin_z, max_x, max_z);
            return true;
        }

        void clear()
        {
            heights.clear();
            heights.shrink_to_fit();
            extra_idx.clear();
            extra_idx.shrink_to_fit();
            extra_vals.clear();
            extra_vals.shrink_to_fit();
            extra_cnt.clear();
            extra_cnt.shrink_to_fit();
            has_multi = false;
            is_cs_format = false;
            origin_x = 0.0f;
            origin_z = 0.0f;
            width = 0;
            depth = 0;
            cell_size = 0.1f;
            data_size = 0;
            max_x = 0.0f;
            max_z = 0.0f;
            max_height = 0.0f;
        }

        bool is_loaded() const
        {
            return !heights.empty();
        }

        bool get_height(float x, float z, float& out_height) const
        {
            if (!is_loaded())
            {
                return false;
            }

            float col_f = (x - origin_x) / cell_size;
            float row_f = (z - origin_z) / cell_size;

            if (col_f < 0.0f || row_f < 0.0f
                || col_f > (float)(width - 1) || row_f > (float)(depth - 1))
            {
                return false;
            }

            int col = (int)col_f;
            int row = (int)row_f;

            if (col >= width - 1)
            {
                col = width - 2;
            }

            if (row >= depth - 1)
            {
                row = depth - 2;
            }

            int idx00 = col * depth + row;
            int idx10 = (col + 1) * depth + row;
            int idx01 = col * depth + (row + 1);
            int idx11 = (col + 1) * depth + (row + 1);
            int16_t v00 = heights[idx00];
            int16_t v10 = heights[idx10];
            int16_t v01 = heights[idx01];
            int16_t v11 = heights[idx11];

            if (v00 == SENTINEL_NO_HIT_I || v10 == SENTINEL_NO_HIT_I
                || v01 == SENTINEL_NO_HIT_I || v11 == SENTINEL_NO_HIT_I)
            {
                return false;
            }

            float h00 = i2f(v00);
            float h10 = i2f(v10);
            float h01 = i2f(v01);
            float h11 = i2f(v11);
            float tx = col_f - (float)col;
            float tz = row_f - (float)row;

            out_height = (h00 + (h10 - h00) * tx)
                + ((h01 + (h11 - h01) * tx) - (h00 + (h10 - h00) * tx)) * tz;
            return true;
        }

        // 取单个 grid cell 的所有层高度，从低到高排列
        int get_cell_all_layers(size_t idx, std::vector<float>& out) const
        {
            out.clear();
            if (idx >= heights.size())
            {
                return 0;
            }
            if (heights[idx] == SENTINEL_NO_HIT_I)
            {
                return 0;
            }

            if (!has_multi)
            {
                out.push_back(i2f(heights[idx]));
                return 1;
            }

            uint8_t lc = extra_cnt[idx];

            if (is_cs_format)
            {
                // C#: extra_vals = 底层 0 .. lc-1, heights = 顶层 lc
                if (lc > 0)
                {
                    uint16_t base = extra_idx[idx];
                    for (uint8_t k = 0; k < lc; ++k)
                    {
                        out.push_back(i2f(extra_vals[base + k]));
                    }
                }
                out.push_back(i2f(heights[idx]));  // 顶层
            }
            else
            {
                // C++ type==3: heights = 底层 0, extra_vals = 层 1 .. lc
                out.push_back(i2f(heights[idx]));  // 底层
                if (lc > 0)
                {
                    uint16_t base = extra_idx[idx];
                    for (uint8_t k = 0; k < lc; ++k)
                    {
                        out.push_back(i2f(extra_vals[base + k]));
                    }
                }
            }
            return (int)out.size();
        }

        // 查询 (x,z) 的所有层高度（单 cell 查询，无插值）
        int get_all_layers_at(float x, float z, std::vector<float>& out) const
        {
            out.clear();
            if (!is_loaded())
            {
                return 0;
            }

            float col_f = (x - origin_x) / cell_size;
            float row_f = (z - origin_z) / cell_size;
            if (col_f < 0.0f || row_f < 0.0f
                || col_f >(float)(width - 1) || row_f >(float)(depth - 1))
            {
                return 0;
            }

            int col = (int)col_f;
            int row = (int)row_f;
            if (col >= width)
            {
                col = width - 1;
            }
            if (row >= depth)
            {
                row = depth - 1;
            }

            size_t idx = (size_t)col * depth + row;
            return get_cell_all_layers(idx, out);
        }

    // 双线性插值版多层查询（与 get_height 的插值方式一致）
    // 对 4 邻域 cell 的同序号层做插值；邻域 cell 缺该层则跳过该层
    int get_all_layers_at_interp(float x, float z, std::vector<float>& out) const
    {
        out.clear();
        if (!is_loaded())
        {
            return 0;
        }

        float col_f = (x - origin_x) / cell_size;
        float row_f = (z - origin_z) / cell_size;
        if (col_f < 0.0f || row_f < 0.0f
            || col_f >(float)(width - 1) || row_f >(float)(depth - 1))
        {
            return 0;
        }

        int col = (int)col_f;
        int row = (int)row_f;
        if (col >= width - 1)
        {
            col = width - 2;
        }
        if (row >= depth - 1)
        {
            row = depth - 2;
        }

        size_t idx00 = (size_t)col * depth + row;
        size_t idx10 = (size_t)(col + 1) * depth + row;
        size_t idx01 = (size_t)col * depth + (row + 1);
        size_t idx11 = (size_t)(col + 1) * depth + (row + 1);

        std::vector<float> c00, c10, c01, c11;
        get_cell_all_layers(idx00, c00);
        get_cell_all_layers(idx10, c10);
        get_cell_all_layers(idx01, c01);
        get_cell_all_layers(idx11, c11);

        // 任一邻域无数据 → 整体无数据（与 get_height 的 NOHIT 检查一致）
        if (c00.empty() || c10.empty() || c01.empty() || c11.empty())
        {
            return 0;
        }

        // 取四邻域最大层数
        size_t max_layers = c00.size();
        if (c10.size() > max_layers)
        {
            max_layers = c10.size();
        }
        if (c01.size() > max_layers)
        {
            max_layers = c01.size();
        }
        if (c11.size() > max_layers)
        {
            max_layers = c11.size();
        }

        float tx = col_f - (float)col;
        float tz = row_f - (float)row;

        for (size_t k = 0; k < max_layers; ++k)
        {
            // 该层必须四个邻域都有，才能插值；缺任一则该层放弃
            bool has_all = (k < c00.size() && k < c10.size() && k < c01.size() && k < c11.size());
            if (!has_all)
            {
                continue;  // 邻域层数不一致 → 跳过该层（保守）
            }

            float h00 = c00[k], h10 = c10[k], h01 = c01[k], h11 = c11[k];
            float h0 = h00 + (h10 - h00) * tx;
            float h1 = h01 + (h11 - h01) * tx;
            out.push_back(h0 + (h1 - h0) * tz);
        }
        return (int)out.size();
    }

    // 按层号取高度：插值优先，4邻域缺该层时降级单cell
    // 返回: true=取到该层高度, false=该层不存在
    bool get_layer_height_interp(float x, float z, int8_t layer, float& out_height) const
    {
        if (!is_loaded() || layer < 0)
        {
            return false;
        }

        // 1) 先尝试插值版
        std::vector<float> layers;
        if (get_all_layers_at_interp(x, z, layers) > 0
            && layer < (int8_t)layers.size())
        {
            out_height = layers[layer];
            return true;
        }

        // 2) 插值版丢失该层（4邻域部分缺）→ 降级单cell
        std::vector<float> single;
        if (get_all_layers_at(x, z, single) > 0
            && layer < (int8_t)single.size())
        {
            out_height = single[layer];   // 精度略低但层号正确
            return true;
        }

        return false;  // 该层确实不存在（真跨层）
    }

    static constexpr float HEIGHT_TOLERANCE = 0.2f;

        // 层感知高度校验 —— 核心方法
        // 返回: 0=同层合法, 1=跨层合法, -1=高度图未加载/无数据, -2=不在任何层容差内
        int get_height_layer_aware(float x, float z, float client_y,
                                   float src_x, float src_z,
                                   int8_t last_layer, float last_y,
                                   float& out_height, int8_t& out_layer,
                                   bool& out_switched) const
        {
            out_switched = false;
            out_height = 0.0f;
            out_layer = last_layer;   // 无数据/拒绝时保留调用方当前层，避免污染 m_hmap_layer

            if (!is_loaded())
            {
                return -1;
            }

            // 1) 插值版获取该 (x,z) 所有层高度
            std::vector<float> candidates;
            if (get_all_layers_at_interp(x, z, candidates) <= 0)
            {
                return -1;
            }

            // 2) 选层：按 last_layer 优先取同层高度（最常见路径：同层移动）
            int8_t  best_layer = -1;
            float   best_diff  = 0.0f;

            if (last_layer >= 0)
            {
                // 优先取同层高度（含降级单cell）
                float same_layer_h;
                if (get_layer_height_interp(x, z, last_layer, same_layer_h))
                {
                    best_layer = last_layer;
                    // 确保 candidates 包含该层（降级单cell时需补入）
                    if (best_layer >= (int8_t)candidates.size())
                    {
                        candidates.resize(best_layer + 1, same_layer_h);
                    }
                    candidates[best_layer] = same_layer_h;
                    best_diff = fabsf(client_y - same_layer_h);
                }
            }

            if (best_layer < 0)
            {
                // 回退：找离 client_y 最近的层（首次进入/真跨层）
                best_layer = 0;
                best_diff  = fabsf(client_y - candidates[0]);
                for (size_t i = 1; i < candidates.size(); ++i)
                {
                    float diff = fabsf(client_y - candidates[i]);
                    if (diff < best_diff)
                    {
                        best_diff  = diff;
                        best_layer = static_cast<int8_t>(i);
                    }
                }
            }

            // 3) 容差校验
            if (best_diff > HEIGHT_TOLERANCE)
            {
                return -2;  // 不在任何层的容差范围内
            }

            out_height = candidates[best_layer];
            out_layer  = best_layer;

            // 4) 跨层检测
            if (last_layer >= 0 && best_layer != last_layer)
            {
                // a) 源点校验：源点 (src_x, src_z) 是否有目标层数据？（插值版）
                std::vector<float> src_layers;
                if (get_all_layers_at_interp(src_x, src_z, src_layers) <= 0)
                {
                    return -2;  // 源点不在高度图范围内
                }

                bool src_has_target = (best_layer < static_cast<int8_t>(src_layers.size()));
                if (src_has_target)
                {
                    // 源点有目标层 → 严格校验高度跳变是否合理
                    float jump = fabsf(out_height - last_y);
                    if (jump <= HEIGHT_TOLERANCE)
                    {
                        // 层号不同但高度差 <= 容差 → 伪跨层，拒绝
                        return -2;
                    }
                }
                // else: 源点无目标层（过渡区/边缘情况）→ 放行
                // 这是正常的分层地图过渡行为（引坡→桥面、楼梯→楼层等）

                out_switched = true;
                return 1;  // 合法跨层
            }

            return 0;  // 同层合法
        }

        void dump_info() const
        {
            fprintf(stdout,
                "\n========== HeightMap Info ==========\n"
                "  Grid:       %d x %d\n"
                "  Cell Size:  %.4f\n"
                "  Origin:     (%.4f, %.4f)\n"
                "  Range:      (%.4f, %.4f) -> (%.4f, %.4f)\n"
                "  Data Bytes: %zu (%.2f MB)\n"
                "  Precision:  1cm (int16_t)\n"
                "  No-Hit Sentinel: %d\n"
                "=====================================\n"
                "\n",
                width, depth, cell_size, origin_x, origin_z,
                origin_x, origin_z, max_x, max_z,
                heights.size() * sizeof(int16_t),
                (float)(heights.size() * sizeof(int16_t)) / (1024.0f * 1024.0f),
                (int)SENTINEL_NO_HIT_I);
        }

        bool dump_to_file(const char* file_path,
            float x_min, float z_min, float x_max, float z_max) const
        {
            if (!is_loaded() || !file_path || !file_path[0])
            {
                return false;
            }

            FILE* fp = fopen(file_path, "w");

            if (!fp)
            {
                fprintf(stderr, "[HeightMap] dump: cannot create %s\n", file_path);
                return false;
            }

            if (x_min < origin_x)
            {
                x_min = origin_x;
            }

            if (z_min < origin_z)
            {
                z_min = origin_z;
            }

            if (x_max > max_x)
            {
                x_max = max_x;
            }

            if (z_max > max_z)
            {
                z_max = max_z;
            }

            int col_start = (int)((x_min - origin_x) / cell_size + 0.5f);
            int row_start = (int)((z_min - origin_z) / cell_size + 0.5f);
            int col_end   = (int)((x_max - origin_x) / cell_size + 0.5f);
            int row_end   = (int)((z_max - origin_z) / cell_size + 0.5f);

            if (col_start < 0)
            {
                col_start = 0;
            }

            if (row_start < 0)
            {
                row_start = 0;
            }

            if (col_end >= width)
            {
                col_end = width - 1;
            }

            if (row_end >= depth)
            {
                row_end = depth - 1;
            }

            int n_cols = col_end - col_start + 1;
            int n_rows = row_end - row_start + 1;
            long long total_pts = (long long)n_cols * n_rows;
            const char* fmt_line = has_multi
                ? "# Format: x,z,layer,height  (layer=0 为底层，递增为上层)\n#\n"
                : "# Format: x,z,height\n#\n";

            fprintf(fp,
                "# HeightMap Dump: %s\n"
                "# Grid: %d x %d  Cell: %.4f  Origin: (%.4f, %.4f)\n"
                "# Range: x=[%.4f..%.4f] z=[%.4f..%.4f]  (%d x %d = %lld points)\n"
                "# Precision: 1cm (int16_t)  No-Hit: filtered out\n"
                "#\n"
                "%s",
                file_path, width, depth, cell_size, origin_x, origin_z,
                x_min, x_max, z_min, z_max, n_cols, n_rows, total_pts, fmt_line);

            long long written = 0;

            for (int col = col_start; col <= col_end; ++col)
            {
                for (int row = row_start; row <= row_end; ++row)
                {
                    size_t cell_idx = (size_t)col * depth + row;
                    int16_t top_h = heights[cell_idx];

                    if (top_h == SENTINEL_NO_HIT_I)
                    {
                        continue;
                    }

                    if (has_multi)
                    {
                        // 多层输出: layer=0 为底层，往上递增
                        uint8_t lc = extra_cnt[cell_idx];

                        if (lc > 0)
                        {
                            uint16_t base = extra_idx[cell_idx];

                            for (uint8_t k = 0; k < lc; ++k)
                            {
                                fprintf(fp, "%.4f,%.4f,%d,%.2f\n",
                                    origin_x + col * cell_size,
                                    origin_z + row * cell_size,
                                    k,                              // layer 0,1,... (底层)
                                    i2f(extra_vals[base + k]));
                                ++written;
                            }
                        }

                        // 顶层在最上面
                        fprintf(fp, "%.4f,%.4f,%d,%.2f\n",
                            origin_x + col * cell_size,
                            origin_z + row * cell_size,
                            lc,                                   // layer=lc 顶层
                            i2f(top_h));
                        ++written;
                    }
                    else
                    {
                        fprintf(fp, "%.4f,%.4f,%.2f\n",
                            origin_x + col * cell_size,
                            origin_z + row * cell_size,
                            i2f(top_h));
                        ++written;
                    }
                }
            }

            fclose(fp);

            fprintf(stdout,
                "\n========== [HeightMap] Dump Complete ==========\n"
                "  File:       %s\n"
                "  Range:      x=[%.4f..%.4f] z=[%.4f..%.4f]\n"
                "  Valid pts:  %lld / %lld  (NOHIT filtered out)\n"
                "================================================\n"
                "\n",
                file_path, x_min, x_max, z_min, z_max, written, total_pts);
            return true;
        }
    };

    struct HeightMapSystem::Impl
    {
        std::map<map_id_t, HeightMap> height_maps;
    };

    HeightMapSystem::HeightMapSystem()
        : impl_(std::make_unique<Impl>())
    {
    }

    HeightMapSystem::~HeightMapSystem() = default;
    HeightMapSystem::HeightMapSystem(HeightMapSystem&&) noexcept = default;
    HeightMapSystem& HeightMapSystem::operator=(HeightMapSystem&&) noexcept = default;

// 扫描目录下所有 *.hgt 文件（自动发现）
    static void scan_heightmap_files(const std::string& scan_dir,
                                 std::vector<std::string>& out_files)
    {
        const std::string ext = ".hgt";
        const size_t ext_len = ext.size();

        try
        {
            for (const auto& entry : std::filesystem::directory_iterator(scan_dir))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }

                auto filename = entry.path().filename().string();
                if (filename.size() < ext_len)
                {
                    continue;
                }

                bool match = true;
                for (size_t i = 0; i < ext_len; ++i)
                {
                    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(filename[filename.size() - ext_len + i])));
                    char b = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[i])));
                    if (a != b)
                    {
                        match = false;
                        break;
                    }
                }

                if (match)
                {
                    out_files.push_back(filename);
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            fprintf(stderr, "[HeightMap] Failed to scan %s: %s\n",
                scan_dir.c_str(), e.what());
        }
    }

    NavStatus HeightMapSystem::load(
        map_id_t map_id,
        const std::filesystem::path& file_path)
    {
        if (file_path.empty())
        {
            return NavStatus::invalid_argument;
        }

        std::error_code error;
        const auto absolute_path = std::filesystem::absolute(file_path, error);
        if (error || !std::filesystem::is_regular_file(absolute_path, error) || error)
        {
            return NavStatus::query_failed;
        }

        HeightMap height_map;
        const std::string path_string = absolute_path.string();
        if (!height_map.load(path_string.c_str()))
        {
            return NavStatus::query_failed;
        }

        impl_->height_maps.insert_or_assign(map_id, std::move(height_map));
        return NavStatus::success;
    }

    NavStatus HeightMapSystem::load_directory(
        const std::filesystem::path& directory,
        std::size_t& loaded_count)
    {
        loaded_count = 0;
        if (directory.empty())
        {
            return NavStatus::invalid_argument;
        }

        std::error_code error;
        const auto absolute_directory = std::filesystem::absolute(directory, error);
        if (error || !std::filesystem::is_directory(absolute_directory, error) || error)
        {
            return NavStatus::query_failed;
        }

        std::vector<std::string> file_names;
        scan_heightmap_files(absolute_directory.string(), file_names);
        std::sort(file_names.begin(), file_names.end());
        for (const std::string& file_name : file_names)
        {
            char* parse_end = nullptr;
            const unsigned long long parsed =
                std::strtoull(file_name.c_str(), &parse_end, 10);
            if (parse_end == file_name.c_str() ||
                parsed > std::numeric_limits<map_id_t>::max())
            {
                continue;
            }

            if (load(
                    static_cast<map_id_t>(parsed),
                    absolute_directory / file_name) == NavStatus::success)
            {
                ++loaded_count;
            }
        }
        return loaded_count > 0 ? NavStatus::success : NavStatus::query_failed;
    }

    bool HeightMapSystem::unload(map_id_t map_id) noexcept
    {
        return impl_->height_maps.erase(map_id) > 0;
    }

    void HeightMapSystem::clear() noexcept
    {
        impl_->height_maps.clear();
    }

    bool HeightMapSystem::is_loaded(map_id_t map_id) const noexcept
    {
        const auto iterator = impl_->height_maps.find(map_id);
        return iterator != impl_->height_maps.end() && iterator->second.is_loaded();
    }

    NavStatus HeightMapSystem::query(
        map_id_t map_id,
        float x,
        float z,
        float& height) const
    {
        if (!std::isfinite(x) || !std::isfinite(z))
        {
            return NavStatus::invalid_argument;
        }
        const auto iterator = impl_->height_maps.find(map_id);
        if (iterator == impl_->height_maps.end() || !iterator->second.is_loaded())
        {
            return NavStatus::heightmap_not_loaded;
        }
        return iterator->second.get_height(x, z, height)
            ? NavStatus::success
            : NavStatus::out_of_range;
    }

    NavStatus HeightMapSystem::query_all_layers(
        map_id_t map_id,
        float x,
        float z,
        std::vector<float>& layers) const
    {
        layers.clear();
        if (!std::isfinite(x) || !std::isfinite(z))
        {
            return NavStatus::invalid_argument;
        }
        const auto iterator = impl_->height_maps.find(map_id);
        if (iterator == impl_->height_maps.end() || !iterator->second.is_loaded())
        {
            return NavStatus::heightmap_not_loaded;
        }
        return iterator->second.get_all_layers_at(x, z, layers) > 0
            ? NavStatus::success
            : NavStatus::out_of_range;
    }

    HeightLayerResult HeightMapSystem::validate_layer(
        map_id_t map_id,
        float x,
        float z,
        float client_y,
        float source_x,
        float source_z,
        std::int8_t last_layer,
        float last_y) const
    {
        HeightLayerResult result;
        result.layer = last_layer;
        if (!std::isfinite(x) ||
            !std::isfinite(z) ||
            !std::isfinite(client_y) ||
            !std::isfinite(source_x) ||
            !std::isfinite(source_z) ||
            !std::isfinite(last_y))
        {
            result.status = NavStatus::invalid_argument;
            return result;
        }

        const auto iterator = impl_->height_maps.find(map_id);
        if (iterator == impl_->height_maps.end() || !iterator->second.is_loaded())
        {
            result.status = NavStatus::heightmap_not_loaded;
            return result;
        }

        const int validation = iterator->second.get_height_layer_aware(
            x,
            z,
            client_y,
            source_x,
            source_z,
            last_layer,
            last_y,
            result.height,
            result.layer,
            result.switched);
        if (validation >= 0)
        {
            result.status = NavStatus::success;
        }
        else if (validation == -1)
        {
            result.status = NavStatus::out_of_range;
        }
        else
        {
            result.status = NavStatus::height_mismatch;
        }
        return result;
    }

    std::optional<HeightMapInfo> HeightMapSystem::get_info(map_id_t map_id) const
    {
        const auto iterator = impl_->height_maps.find(map_id);
        if (iterator == impl_->height_maps.end() || !iterator->second.is_loaded())
        {
            return std::nullopt;
        }

        const HeightMap& height_map = iterator->second;
        return HeightMapInfo{
            height_map.origin_x,
            height_map.origin_z,
            height_map.width,
            height_map.depth,
            height_map.cell_size,
            height_map.max_height,
        };
    }

    void HeightMapSystem::dump_info(map_id_t map_id) const
    {
        const auto iterator = impl_->height_maps.find(map_id);
        if (iterator != impl_->height_maps.end())
        {
            iterator->second.dump_info();
        }
    }

    bool HeightMapSystem::dump_to_file(
        map_id_t map_id,
        const std::filesystem::path& file_path,
        float x_min,
        float z_min,
        float x_max,
        float z_max) const
    {
        const auto iterator = impl_->height_maps.find(map_id);
        if (iterator == impl_->height_maps.end() || file_path.empty())
        {
            return false;
        }
        const std::string path_string = file_path.string();
        return iterator->second.dump_to_file(
            path_string.c_str(),
            x_min,
            z_min,
            x_max,
            z_max);
    }
} // namespace game::navigation
