# VFS Enhancements — Design Spec

## Overview

Three enhancements to the tmpfs filesystem server:

1. **Nested paths** — support `/foo/bar.txt` by walking directory hierarchy
2. **readdir** — list directory contents
3. **Large files** — support files > 4096 bytes via multiple VMO pages

## 1. Nested Paths

### Current state

`find_entry` searches a single flat linked list from root. Only flat names work.

### Design

Add hierarchical directories. Each directory has its own DirEntry chain (`children`).

**Data structure change:**

```cpp
struct DirEntry {
    char     name[256];
    uint32_t vmo_handle;
    bool     is_dir;
    uint64_t size;
    DirEntry* next;       // sibling (same directory)
    DirEntry* children;   // first child (only if is_dir)
};
```

**Path resolution in `_start()`:**

Split path by `/`, walk from root through children chains. If a component doesn't exist and O_CREAT is set, create the intermediate directory.

```cpp
DirEntry* resolve_path(const char* path, bool create_dirs) {
    DirEntry* current = root_dir;  // pre-created root directory
    if (*path == '/') path++;

    while (*path) {
        // Extract next component
        char comp[256];
        int ci = 0;
        while (*path && *path != '/') comp[ci++] = *path++;
        comp[ci] = '\0';
        if (*path == '/') path++;

        // Search current directory's children
        DirEntry* child = find_in_dir(current, comp);
        if (!child) {
            if (!create_dirs) return nullptr;
            // Create as directory if path continues, file if last
            bool is_last = (*path == '\0');
            child = create_entry(current, comp, is_last);
            if (!child) return nullptr;
        }
        current = is_last ? child : (child->is_dir ? child : nullptr);
    }
    return current;
}
```

### Files modified

- `kernel/fs/tmpfs/tmpfs.cpp` — DirEntry struct, _start() path resolution, add_entry signature

## 2. readdir

### Current state

Readdir returns -1 (unimplemented).

### Design

In `handle_file()`, when Readdir is received on a directory file:

1. Walk `entry->children` chain
2. For each child, fill a `Dirent` struct (name, type, size)
3. Pack `Dirent[]` array into response data
4. Return with `FileResponse{0, count*sizeof(Dirent)}`

The dirent cookie (offset in FileMsg) is used for pagination — start from the Nth child.

```cpp
case FileMsg::Readdir: {
    if (!fs->is_dir) { resp.result = -1; break; }
    uint32_t cookie = msg->offset;  // start index
    uint32_t count = 0;
    Dirent* dirs = alloc_temp_dirents();
    for (DirEntry* e = entry->children; e; e = e->next) {
        if (cookie > 0) { cookie--; continue; }
        Dirent d;
        d.type = e->is_dir ? 1 : 0;
        d.size = e->size;
        // copy name
        for (int i = 0; i < 256; i++) d.name[i] = e->name[i];
        dirs[count++] = d;
        if (count >= max_dirents) break;
    }
    // send response + dirent array
    resp.result = 0;
    resp.size = count * sizeof(Dirent);
    channel_write(fs->file_chan, &resp, sizeof(resp));
    channel_write_raw(fs->file_chan, dirs, resp.size); // need a way to send data with response
    break;
}
```

### Files modified

- `kernel/fs/tmpfs/tmpfs.cpp` — handle_file Readdir case

## 3. Large Files (> 4096 bytes)

### Current state

`vmo_create(4096)` on file creation. Write clamps to 4096. Read only from first page.

### Design

Multi-page VMO management in FileState:

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

**Page allocation on write:**

```cpp
case FileMsg::Write: {
    size_t offset = msg->offset;
    size_t remaining = data_len;
    size_t written = 0;

    while (remaining > 0) {
        int page_idx = offset / 4096;
        if (page_idx >= MAX_PAGES) break;

        if (page_idx >= fs->page_count) {
            // Allocate new page VMO
            uint32_t vmo = vmo_create(4096);
            if (!vmo) break;
            fs->vmo_pages[page_idx] = vmo;
            fs->page_count = page_idx + 1;
            vmo_map(vmo, MAP_BASE, 3, 0);
        } else {
            vmo_map(fs->vmo_pages[page_idx], MAP_BASE, 3, 0);
        }

        size_t page_off = offset % 4096;
        size_t chunk = 4096 - page_off;
        if (chunk > remaining) chunk = remaining;

        uint8_t* dst = (uint8_t*)MAP_BASE + page_off;
        for (size_t i = 0; i < chunk; i++) dst[i] = data[i];
        written += chunk; remaining -= chunk; offset += chunk;

        if (fs->entry && offset > fs->entry->size)
            fs->entry->size = offset;
    }
    resp.result = 0; resp.size = written;
    break;
}
```

**Read across pages:**

```cpp
case FileMsg::Read: {
    // similar loop: for each page in range, map VMO, copy to output buffer
}
```

### Files modified

- `kernel/fs/tmpfs/tmpfs.cpp` — FileState struct, handle_file Read/Write cases

## Verification

```bash
bash scripts/run.sh
```

Init test extended:
- Test 8: create `/foo/bar.txt` (nested path)
- Test 9: write > 4096 bytes to a file, read back
- Test 10: readdir listing of root and subdirectories
