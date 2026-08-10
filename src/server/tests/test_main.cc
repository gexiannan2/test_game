// 测试可执行文件入口；GAME_TEST_FILTER 按子串过滤用例。
#include "test_harness.h"

int main() {
    return ::test::Runner::Instance().RunAll();
}
