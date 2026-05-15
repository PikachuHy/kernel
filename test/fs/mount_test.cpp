#include <gtest/gtest.h>
#include "kernel/fs/mount.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/object/process.hpp"

TEST(MountTest, InitEmpty) {
    mount_init();
    EXPECT_EQ(mount_resolve("/anything"), nullptr);
}

TEST(MountTest, AddAndResolveExact) {
    mount_init();
    mount_add("/dev", nullptr, nullptr);
    MountEntry* m = mount_resolve("/dev");
    EXPECT_NE(m, nullptr);
    // Can't use EXPECT_STREQ without libc in kernel build.
    // Check first few chars manually.
    EXPECT_EQ(m->path[0], '/');
    EXPECT_EQ(m->path[1], 'd');
    EXPECT_EQ(m->path[2], 'e');
    EXPECT_EQ(m->path[3], 'v');
    EXPECT_EQ(m->path[4], '\0');
}

TEST(MountTest, AddAndResolvePrefix) {
    mount_init();
    mount_add("/dev", nullptr, nullptr);
    MountEntry* m = mount_resolve("/dev/console");
    EXPECT_NE(m, nullptr);
    EXPECT_EQ(m->path[0], '/');
    EXPECT_EQ(m->path[1], 'd');
}

TEST(MountTest, LongestPrefixMatch) {
    mount_init();
    mount_add("/", nullptr, nullptr);
    mount_add("/dev", nullptr, nullptr);

    MountEntry* m = mount_resolve("/dev/console");
    EXPECT_NE(m, nullptr);
    EXPECT_EQ(m->path[1], 'd');  // "/dev" not "/"

    MountEntry* m2 = mount_resolve("/foo/bar");
    EXPECT_NE(m2, nullptr);
    EXPECT_EQ(m2->path[1], '\0');  // "/"
}

TEST(MountTest, NoMatchWithoutSlash) {
    mount_init();
    mount_add("/dev", nullptr, nullptr);
    MountEntry* m = mount_resolve("/device");
    EXPECT_EQ(m, nullptr);
}

TEST(MountTest, Remove) {
    mount_init();
    mount_add("/dev", nullptr, nullptr);
    EXPECT_NE(mount_resolve("/dev"), nullptr);
    mount_remove("/dev");
    EXPECT_EQ(mount_resolve("/dev"), nullptr);
}

TEST(MountTest, ReplaceExisting) {
    mount_init();
    mount_add("/dev", nullptr, nullptr);
    mount_add("/dev", nullptr, nullptr);
    EXPECT_NE(mount_resolve("/dev"), nullptr);
}

TEST(MountTest, FullTable) {
    mount_init();
    char buf[32];
    for (int i = 0; i < 16; i++) {
        buf[0] = '/'; buf[1] = 'm';
        buf[2] = '0' + (i % 10);
        buf[3] = '\0';
        int rc = mount_add(buf, nullptr, nullptr);
        if (i < 15) EXPECT_EQ(rc, 0);
    }
}
