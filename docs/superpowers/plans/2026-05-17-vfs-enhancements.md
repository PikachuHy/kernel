# VFS Enhancements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add nested paths, readdir, and large file (>4096 byte) support to tmpfs.

**Architecture:** All changes are in tmpfs.cpp. DirEntry gains `children` linked list for directory hierarchy. FileState gains `vmo_pages[]` array for multi-page files. Path resolution walks `children` chains. Readdir iterates children. Write/Read span pages.

**Tech Stack:** C++26 freestanding, ring-3 ELF at 0x600000

---

## File Structure

```
kernel/fs/tmpfs/tmpfs.cpp   — DirEntry.children, FileState.vmo_pages[], _start() path walk, 
                               handle_file Readdir + multi-page Read/Write
kernel/init/init.cpp         — add tests for nested paths, large files, readdir
```

---

### Task 1: Nested Paths + readdir

**Files:**
- Modify: `kernel/fs/tmpfs/tmpfs.cpp`

- [ ] **Step 1: Add `children` field to DirEntry**

Edit the DirEntry struct (around line 72-78):

```cpp
struct DirEntry {
    char     name[256];
    uint32_t vmo_handle;
    bool     is_dir;
    uint64_t size;
    DirEntry* next;       // sibling in same directory
    DirEntry* children;   // first child (only if is_dir)
};
```

- [ ] **Step 2: Add `find_in_dir` and rewrite `_start` path resolution**

Replace `find_entry` with a directory-aware version. Rewrite `_start()` to walk path components:

```cpp
// Search a directory's children for a name match
static DirEntry* find_in_dir(DirEntry* dir, const char* name) {
    if (!dir || !dir->is_dir) return nullptr;
    for (DirEntry* e = dir->children; e; e = e->next) {
        int i = 0;
        while (name[i] && e->name[i] && name[i] == e->name[i]) i++;
        if (name[i] == '\0' && e->name[i] == '\0') return e;
    }
    return nullptr;
}
```

Create a root directory entry at startup:

```cpp
static DirEntry* root_dir = nullptr;

// In _start(), before the main loop:
static bool initialized = false;
if (!initialized) {
    root_dir = &pool[pool_idx++];
    root_dir->is_dir = true;
    root_dir->name[0] = '\0';
    root_dir->children = nullptr;
    root_dir->next = nullptr;
    initialized = true;
}
```

Replace the simple `find_entry(path)` with path walk in `_start()`:

```cpp
// Path resolution: walk components, create dirs if needed
const char* rel = payload->path;
while (*rel == '/') rel++;

// Walk from root
DirEntry* current = root_dir;
DirEntry* entry = nullptr;
char comp[256];
int ci = 0;

while (*rel) {
    // Extract next path component
    ci = 0;
    while (*rel && *rel != '/') { comp[ci++] = *rel; rel++; }
    comp[ci] = '\0';
    if (*rel == '/') rel++;

    if (comp[0] == '\0') continue;  // skip empty (leading slash)

    bool is_last = (*rel == '\0');
    DirEntry* child = find_in_dir(current, comp);

    if (!child) {
        if (!(payload->flags & O_CREAT) || !is_last) {
            // Not found and can't create
            entry = nullptr;
            break;
        }
        // Create new file
        uint32_t vmo = vmo_create(4096);
        if (!vmo) break;
        child = alloc_dir_entry(current, comp, vmo, false, 0);
    }

    current = child;
    if (is_last) entry = child;
}
```

- [ ] **Step 3: Add `alloc_dir_entry` helper**

```cpp
// Allocate a DirEntry and add to parent directory's children list
static DirEntry* alloc_dir_entry(DirEntry* parent, const char* name,
                                  uint32_t vmo_h, bool is_dir, uint64_t sz) {
    if (pool_idx >= 64) return nullptr;
    DirEntry* e = &pool[pool_idx++];
    int i = 0;
    while (name[i] && i < 255) { e->name[i] = name[i]; i++; }
    e->name[i] = '\0';
    e->vmo_handle = vmo_h;
    e->is_dir = is_dir;
    e->size = sz;
    e->next = nullptr;
    e->children = nullptr;
    // Add to parent's children list
    if (parent && parent->is_dir) {
        e->next = parent->children;
        parent->children = e;
    }
    return e;
}
```

Remove the old `add_entry` and `find_entry` functions (keep code clean).

- [ ] **Step 4: Implement readdir in handle_file**

In `handle_file()`, replace the Readdir case:

```cpp
case FileMsg::Readdir: {
    if (!fs->is_dir || !fs->entry) {
        resp.result = -1;
        channel_write(fs->file_chan, &resp, sizeof(resp));
        break;
    }
    // Collect dirent list, pack into response
    uint8_t out[sizeof(FileResponse) + 4096];
    size_t count = 0;
    const size_t max_dirents = (4096 - sizeof(FileResponse)) / sizeof(Dirent);
    Dirent* dirents = reinterpret_cast<Dirent*>(out + sizeof(FileResponse));

    uint32_t skip = msg->offset;  // cookie for pagination
    for (DirEntry* e = fs->entry->children; e && count < max_dirents; e = e->next) {
        if (skip > 0) { skip--; continue; }
        dirents[count].type = e->is_dir ? 1 : 0;
        dirents[count].size = e->size;
        int j = 0;
        while (e->name[j] && j < 255) { dirents[count].name[j] = e->name[j]; j++; }
        dirents[count].name[j] = '\0';
        count++;
    }
    FileResponse* rp = reinterpret_cast<FileResponse*>(out);
    rp->result = 0;
    rp->size = count * sizeof(Dirent);
    channel_write(fs->file_chan, out, sizeof(FileResponse) + rp->size);
    break;
}
```

- [ ] **Step 5: Build and QEMU test**

```bash
bazel build //kernel:kernel && bash scripts/run.sh 2>&1 | grep -E "PASS|FAIL|=== init|ALL TESTS"
```
Expected: build succeeds, existing tests pass.

- [ ] **Step 6: Add init test for nested paths + readdir**

Edit `kernel/init/init.cpp`, add test 8:

```cpp
pr("8. nested path: ");
uint64_t fh2 = s6(SYS_OPEN, (uint64_t)"/a/b/test.txt", 0xC, 0, 0, 0);
if (fh2 == INV || fh2 == 0) T_FAIL("nested path", "create fail");
else {
    // Write data
    uint8_t wb[40]; FileMsg* w = (FileMsg*)wb;
    w->op = FileMsg::Write; w->flags = 0; w->offset = 0; w->length = 6;
    const char* dd = "nested";
    for (int i = 0; i < 6; i++) wb[24+i] = (uint8_t)dd[i];
    cw(fh2, wb, 30); FileResponse wr2; cr(fh2, &wr2, sizeof(wr2));
    // Read back
    w->op = FileMsg::Read; w->length = 64;
    cw(fh2, w, sizeof(FileMsg));
    uint8_t rb2[80]; cr(fh2, rb2, sizeof(rb2));
    FileResponse* rr2 = (FileResponse*)rb2;
    if (rr2->size == 6) T_OK("nested path create+read");
    else T_FAIL("nested path", "bad read");
    cl(fh2);
}
```

- [ ] **Step 7: Commit**

```bash
git add kernel/fs/tmpfs/tmpfs.cpp kernel/init/init.cpp
git commit -m "feat: nested paths and readdir support in tmpfs"
```

---

### Task 2: Large Files (> 4096 bytes)

**Files:**
- Modify: `kernel/fs/tmpfs/tmpfs.cpp`

- [ ] **Step 1: Change FileState to multi-page**

Edit FileState struct (around line 108-116):

```cpp
constexpr int MAX_PAGES = 16;  // 64KB max per file

struct FileState {
    uint32_t  file_chan;
    uint32_t  vmo_pages[MAX_PAGES];  // one VMO handle per 4KB page
    int       page_count;
    uint64_t  cursor;
    bool      is_dir;
    DirEntry* entry;
    FileState* next;
};
```

- [ ] **Step 2: Rewrite handle_file Read to span pages**

Replace the Read case:

```cpp
case FileMsg::Read: {
    size_t offset = msg->offset;
    size_t total = msg->length;
    if (total > 4096 - sizeof(FileResponse))
        total = 4096 - sizeof(FileResponse);

    uint8_t out[sizeof(FileResponse) + 4096];
    size_t read_bytes = 0;

    for (int i = 0; i < fs->page_count && read_bytes < total; i++) {
        size_t page_base = i * 4096;
        if (offset >= page_base + 4096) continue;
        size_t page_off = (offset < page_base) ? 0 : (offset - page_base);
        size_t chunk = 4096 - page_off;
        if (chunk > total - read_bytes) chunk = total - read_bytes;
        if (offset + chunk > file_size) chunk = file_size > offset ? file_size - offset : 0;

        if (chunk > 0 && fs->vmo_pages[i]) {
            vmo_map(fs->vmo_pages[i], MAP_BASE, 3, 0);
            uint8_t* src = (uint8_t*)MAP_BASE + page_off;
            for (size_t j = 0; j < chunk; j++)
                out[sizeof(FileResponse) + read_bytes + j] = src[j];
            read_bytes += chunk;
            offset += chunk;
        }
    }
    *(FileResponse*)out = {0, read_bytes};
    channel_write(fs->file_chan, out, sizeof(FileResponse) + read_bytes);
    break;
}
```

- [ ] **Step 3: Rewrite handle_file Write to extend across pages**

Replace the Write case:

```cpp
case FileMsg::Write: {
    size_t offset = msg->offset;
    size_t remaining = data_len;
    size_t written = 0;

    while (remaining > 0 && (int)(offset / 4096) < MAX_PAGES) {
        int pi = offset / 4096;
        // Allocate page VMO if needed
        if (pi >= fs->page_count || !fs->vmo_pages[pi]) {
            uint32_t vmo = vmo_create(4096);
            if (!vmo) break;
            fs->vmo_pages[pi] = vmo;
            if (pi >= fs->page_count) fs->page_count = pi + 1;
            vmo_map(vmo, MAP_BASE, 3, 0);
        } else {
            vmo_map(fs->vmo_pages[pi], MAP_BASE, 3, 0);
        }

        size_t page_off = offset % 4096;
        size_t chunk = 4096 - page_off;
        if (chunk > remaining) chunk = remaining;

        uint8_t* dst = (uint8_t*)MAP_BASE + page_off;
        for (size_t i = 0; i < chunk; i++) dst[i] = data[i];
        written += chunk; remaining -= chunk; offset += chunk; data += chunk;
    }
    if (fs->entry && offset > (uint64_t)fs->entry->size)
        fs->entry->size = offset;
    resp.result = 0;
    resp.size = written;
    channel_write(fs->file_chan, &resp, sizeof(resp));
    break;
}
```

- [ ] **Step 4: Update alloc_file_state to init page array**

```cpp
static FileState* alloc_file_state(uint32_t fchan, uint32_t vmo, bool dir, DirEntry* e) {
    if (fspool_idx >= 64) return nullptr;
    FileState* fs = &fspool[fspool_idx++];
    fs->file_chan = fchan;
    for (int i = 0; i < MAX_PAGES; i++) fs->vmo_pages[i] = 0;
    fs->page_count = 0;
    fs->cursor = 0;
    fs->is_dir = dir;
    fs->entry = e;
    fs->next = file_list;
    file_list = fs;
    return fs;
}
```

- [ ] **Step 5: Build and QEMU test**

```bash
bazel build //kernel:kernel && bash scripts/run.sh 2>&1 | grep -E "PASS|FAIL|=== init|ALL TESTS"
```

- [ ] **Step 6: Add init test for large file (> 4096 bytes)**

Edit `kernel/init/init.cpp`, add test 9:

```cpp
pr("9. large file: ");
uint64_t lfh = s6(SYS_OPEN, (uint64_t)"/large.bin", 0xC, 0, 0, 0);
if (lfh == INV || lfh == 0) T_FAIL("large file", "create fail");
else {
    // Write 5000 bytes (spans 2 pages)
    uint8_t wb[sizeof(FileMsg) + 5000];
    FileMsg* w = (FileMsg*)wb;
    w->op = FileMsg::Write; w->flags = 0; w->offset = 0; w->length = 5000;
    for (int i = 0; i < 5000; i++) wb[sizeof(FileMsg)+i] = (uint8_t)(i & 0xFF);
    cw(lfh, wb, sizeof(FileMsg) + 5000);
    FileResponse wr3; cr(lfh, &wr3, sizeof(wr3));
    // Read page 2 (offset 4096, length 1000)
    w->op = FileMsg::Read; w->flags = 0; w->offset = 4096; w->length = 1000;
    cw(lfh, w, sizeof(FileMsg));
    uint8_t rb3[sizeof(FileResponse)+2000]; cr(lfh, rb3, sizeof(rb3));
    FileResponse* rr3 = (FileResponse*)rb3;
    if (rr3->size == 1000 && rb3[16] == 0 && rb3[17] == 1)
        T_OK("large file multi-page rw");
    else T_FAIL("large file", "bad multi-page");
    cl(lfh);
}
```

- [ ] **Step 7: Commit**

```bash
git add kernel/fs/tmpfs/tmpfs.cpp kernel/init/init.cpp
git commit -m "feat: multi-page VMO support for large files in tmpfs"
```

---

### Task 3: Final Verification

- [ ] **Step 1: Run host tests**

```bash
bazel test //test/fs:all //test/irq:irq_test //test/mm:all //test/sched:sched_test --test_output=errors
```
Expected: 8/8 pass.

- [ ] **Step 2: Run QEMU boot test**

```bash
bash scripts/run.sh 2>&1 | grep -E "PASS|FAIL|=== Results|ALL TESTS"
```
Expected: 9/9 tests pass (including nested path + large file).

- [ ] **Step 3: Update CLAUDE.md**

Mark VFS enhancements as done.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: mark VFS enhancements as done"
```
