#include <gtest/gtest.h>
#include <stdint.h>
#include <stddef.h>
#include "kernel/fs/protocol.hpp"

TEST(ProtocolTest, FileMsgSize) {
    EXPECT_EQ(sizeof(FileMsg), 24u);  // op(4) + flags(4) + offset(8) + length(8)
}

TEST(ProtocolTest, FileResponseSize) {
    EXPECT_EQ(sizeof(FileResponse), 16u);  // result(4) + padding(4) + size(8)
}

TEST(ProtocolTest, StatSize) {
    EXPECT_EQ(sizeof(Stat), 16u);
}

TEST(ProtocolTest, DirentSize) {
    // char[256] + uint32_t + padding(4) + uint64_t = 272
    EXPECT_EQ(sizeof(Dirent), 272u);
}

TEST(ProtocolTest, OpenFlags) {
    EXPECT_EQ(O_RDONLY, 1u << 0);
    EXPECT_EQ(O_WRONLY, 1u << 1);
    EXPECT_EQ(O_RDWR,   1u << 2);
    EXPECT_EQ(O_CREAT,  1u << 3);
}
