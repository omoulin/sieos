# SieFS: the SIEOS file system

SieFS keeps files on a disk so that they survive anything: a crash or
power cut at any moment leaves either the state of the last commit or the
one before it, never a mix, and every block read is checked against a
checksum, so damage is detected rather than silently returned.

## How it works, in five ideas

1. **Never overwrite.** A change is written to free space. The old version
   stays untouched until the change is safely recorded.
2. **One tree holds everything.** Inodes, directory entries, file extents,
   small files and attributes are *items* in one B+tree, sorted by a key
   `(object id, type, offset)`. All the items of one file sit next to each
   other.
3. **A commit flips one block.** Changes collect in memory. A commit
   writes the new tree nodes, then the free-space bitmap, then a
   superblock pointing at all of it. Until that single superblock lands,
   the disk still describes the old state.
4. **Parents hold their children's checksums.** Each pointer to a block
   carries the block's CRC-32C. Starting from the superblock, every node
   and every data block is checked on its way in (a "Merkle tree").
5. **Small files live inside the tree.** A file of up to 1536 bytes is
   stored in its own item, next to its inode: reading it costs no extra
   block. Most configuration files, scripts and notes are this small.

These ideas come from a long line of research and systems: shadow paging,
copy-on-write B-trees, checksums in block pointers, write-once commits
with alternating superblocks. SieFS combines them in about 2,000 lines of C.

## Disk layout

Blocks are 4 KiB, numbers are little-endian.

```
 block 0         unused (room for a boot record)
 block 1         superblock, slot 0  ┐ commit s writes slot s % 2:
 block 2         superblock, slot 1  ┘ the previous commit's slot is untouched
 idx[0]          bitmap copy 0: index (CRC of each bitmap block)
 bm[0]           bitmap copy 0: 1 bit per block, 1 = in use
 idx[1], bm[1]   the same, copy 1
 data_start ...  tree nodes and file data, anywhere
```

A bitmap block covers 32768 blocks (128 MiB); an index block holds 1024
checksums. A 256 MiB volume needs 11 blocks of layout in all (0.02 %).

### The superblock (`sb_t`)

| field | meaning |
|---|---|
| `magic`, `version`, `bs` | `"SIEFSv1\0"`, 1, 4096 |
| `nblocks` | size of the volume |
| `seq` | commit number; slot = `seq % 2` |
| `root` | block pointer to the **root tree** |
| `bm[2]`, `idx[2]`, `nbm`, `nidx`, `data_start` | the layout above |
| `used`, `created`, `uuid`, `label` | information |
| `idxsum[900]` | CRC of each index block of *this slot's* bitmap copy (so volumes up to 113 TiB) |
| `csum` | CRC-32C of the whole block (with this field 0) |

**Mounting:** read both slots. Take the valid one with the highest
`seq`, load its bitmap copy (checking index and blocks), its root tree and
the volume record. If anything fails, use the other slot: the previous
commit, intact.

### Block pointers (`bptr_t`, 40 bytes)

```
 blk (8)  gen (8)  csum (4)  flags (4)  mac (16)
```

`gen` is the commit that wrote the block (checked on read: a stale block
cannot pass for a newer one). `csum` is the block's CRC-32C. `flags` and
`mac` are reserved for authenticated encryption (see below).

### Tree nodes (one block)

```
 header (32): magic "NODE" | level | n | dstart | gen | tree
 leaf:     [item 0][item 1]...      free      ...[value 1][value 0]
           item = key (16) + offset (2) + length (2) + pad   (24 bytes)
 internal: [entry 0][entry 1]...    entry = key (16) + bptr (40)   (56 bytes, 72 per node)
```

An internal entry's key is the first key of its child. Values are at most
1900 bytes, so any two items fit in a node.

### Keys and items

A key is `(id, type << 56 | offset)`, compared as two unsigned numbers.

| type | offset | value |
|---|---|---|
| `INODE` 1 | 0 | mode, uid, gid, nlink, size, parent, flags, times (ns), 96 bytes |
| `INLINE` 2 | 0 | the whole contents of a small file or a symlink target |
| `EXTENT` 3 | byte offset in the file | first disk block, block count (≤ 32), flags, one CRC per block |
| `DIRENT` 4 | 56-bit hash of the name | list of `ino (8) · type (1) · name length (1) · name` |
| `XATTR` 5 | 56-bit hash of the name | list of `name length (1) · value length (2) · name · value` |
| `XINDEX` 6 | object id | none; the id is `1 << 63 \| hash(name, value)` |
| `VOLUME` 16 | 0 | in the root tree: the volume's file tree, next id, object count, key id |

Objects (files, directories, symlinks) have ids that are never reused;
the root directory is object 2. Names that hash alike share one item.

### Two levels of trees

The superblock points at a **root tree** whose items are **volumes**,
each with its own **file tree**. Version 1 has one volume (id 1). The
level exists so that later each user's home can be its own volume, with
its own encryption key and its own snapshots, without changing the format.

## The commit, step by step

```
 1. write every dirty node to a new block, children before parents
    (consecutive nodes in one write), each parent recording the new
    block, commit number and CRC of its children; then the root tree
 2. flush             file data (written by the operations) and nodes are on disk
 3. write this slot's bitmap copy (only the blocks that changed since this
    copy was last written) and its index; flush
 4. write this slot's superblock; flush      <- the commit happens here
 5. release the blocks freed by the previous transaction
```

Every operation (create, write, rename...) lands whole in one commit:
commits only happen between operations. They happen on `siefs_sync`, past
`dirty_limit` changed nodes (256, i.e. 1 MiB), or `commit_ns` after the
first change (5 s).

## Free space

The bitmap in memory has three versions of each bit:

- **map**: referenced by the trees, which is what the next commit writes;
- **busy**: not allocatable (map, plus blocks freed less than two commits ago);
- **fresh**: allocated in this transaction.

When a block stops being used: if it is *fresh*, nothing on disk knows it,
so it is free at once. Otherwise it waits: a block freed by transaction
`t` becomes allocatable after commit `t + 1`. So the trees of *both*
superblocks on disk stay intact at all times: the newest, and the one
before as a fallback.

Why a bitmap kept in two fixed copies, rather than free space stored in
the tree itself? Storing it in the tree means allocating blocks changes
the tree that records allocations, a loop that needs careful tricks to
end. Two fixed copies, one per superblock slot, are simple and just as
safe: a commit only writes the copy of the slot it is about to take over,
so the other slot's copy stays valid. The cost is small: 32 KiB per GiB,
and only changed bitmap blocks are written.

Allocation is next-fit (it continues after the last allocation) and asks
for runs of up to 32 blocks, so files and the nodes of a commit land
together.

## Checksums

CRC-32C, computed with tables 8 bytes at a time (no special instructions:
the code runs anywhere). Every node and data block is checked when read;
superblocks, bitmap blocks and index blocks too. A mismatch is `-EIO`,
never silently wrong data. When the newest commit is damaged, mounting
falls back to the previous one.

## Extended attributes and the attribute index

Any object can carry attributes, for example `project = SIEOS`. Each
attribute whose value is at most 255 bytes also gets an index item
`(1 << 63 | hash(name, value), XINDEX, object id)`. Finding every object
with `project = SIEOS` reads only the matching index items, in order, and
checks each candidate's real value (hashes can collide). This is what the
desktop needs for projects and live queries.

## Encryption (planned, the format is ready)

- **Unit:** one key per volume, so one per user's home. The key is derived
  from the user's password with Argon2id, and wraps a random volume key
  (so changing a password rewrites one record).
- **Cipher:** XChaCha20-Poly1305 (fast without special CPU instructions)
  or AES-256-GCM, chosen per volume (`flags`).
- **Authenticated encryption:**
  - Every node and data block is encrypted whole. Its 16-byte tag goes in
    the parent's pointer (`mac`), next to the CRC.
  - The nonce comes from the block number and the commit number. Copy-on-
    write never writes the same block twice in one commit, so a nonce is
    never reused.
  - Tampering with the disk, even by someone who copies old blocks back,
    is detected, not just garbled.
- **What stays visible:**
  - The root tree and the bitmap: block usage, not contents.
  - In encrypted volumes, names are hashed with a keyed hash (SipHash), so
    the tree's order reveals nothing.

## Memory and speed

| knob (`siefs_env_t`) | default | meaning |
|---|---|---|
| `cache_nodes` | 128 (512 KiB) | clean tree nodes kept in memory |
| `dirty_limit` | 256 (1 MiB) | changed nodes before an automatic commit |
| `commit_ns` | 5 s | time before an automatic commit |

A mounted volume uses about 250 KiB plus the cache:
- **Bitmaps:** 3 bits per block, i.e. 96 KiB per GiB.
- **Data and commit buffer:** one 128 KiB buffer, shared by file data and
  the commit's node writes.
- **Hash table and tables:** small fixed tables.

File data is not cached by the library; that is the server's choice.

Measured on an in-memory disk (`make siefs-test`), so this is the cost of
SieFS itself on one core:

| | |
|---|---|
| core library, freestanding | 39 KB of code at -O2, 30 KB at -Os (the checker is 8 KB of it) |
| create | ~500,000 files/s (20,000 in one directory) |
| lookup | ~630,000/s |
| small files (1000 bytes, inline) | ~245,000/s |
| large file | write ~1.2 GB/s, read ~1.6 GB/s, checksums included |
| mount | 7 blocks read, 0.03 ms |
| path lookup in a 20,000-file directory | 5 blocks read cold, 2 warm |
| empty 256 MiB volume | 44 KiB used |

## Limits of version 1

- **Sizes:** files up to 2^56 bytes, volumes up to 113 TiB, names up to
  255 bytes, attribute values up to 1024 bytes.
- **Concurrency:** one thread at a time. The server serializes requests.
- **Unlinked files:** none. A file disappears with its last name. A server
  that wants POSIX "open but unlinked" files keeps them itself.
- **Access times:** not updated (`atime` is set at creation, or by
  `siefs_setattr`).
- **Directory listings:** in hash order. A cursor stays valid across
  insertions; removing a name that shares a hash bucket can make a running
  listing skip one entry.
- **Not yet:** snapshots, quotas, encryption (the format has room for
  all of them), several volumes.
- **Errors:**
  - An operation that fails *after* it started changing things (only a
    device error or no memory can do that) stops the mount
    (`-EIO` from then on).
  - The disk still holds the last commit, so mounting again returns to it.

## The code

| file | what |
|---|---|
| `siefs/siefs.h` | the interface: what the SIEOS file server and the tools call |
| `siefs/siefs_int.h` | the on-disk formats and in-memory state |
| `siefs/tree.c` | the copy-on-write B+tree, the node cache, the commit's node writes |
| `siefs/space.c` | free space, the commit, format, mount |
| `siefs/fs.c` | files, directories, links, rename, attributes |
| `siefs/check.c` | `siefs_check`, the full verification |
| `siefs/crc.c` | CRC-32C |

The library needs no operating system: the caller passes block read,
write and flush functions, memory allocation and a clock. It compiles
freestanding with the SIEOS flags (`make siefs-freestanding` checks that
nothing but `memcpy` & co. and `vformat` is needed).

## Host tools

Programs for the development machine, built with `make siefs-tools` into
`build/host/`. They are written in ISO C (`tools/siefs/`). The one part
that cannot be, walking a host directory for `mkfs.siefs -d`, is isolated
in `tools/hostdir.c`. The same tool sources are meant to build as SIEOS
programs later, so that SIEOS can create and check its own disks
(self-hosting).

```
mkfs.siefs -s 256M -L SIEOS -d rootfs/ disk.img     make a volume, copy a directory in
fsck.siefs -d disk.img                              check everything (-d: data blocks too)
siefs-dump disk.img                                 show the superblocks and every node
siefs disk.img ls /         siefs disk.img cat /etc/motd
siefs disk.img put notes.txt /home/notes.txt
siefs disk.img attr /home/notes.txt project SIEOS
siefs disk.img find project SIEOS                   every object with that attribute
```

## Tests

`make siefs-test` runs:
- **Unit tests** of every operation and its errors.
- **A model test:** 12,000 random operations compared against an
  in-memory model, with a full check after every commit and remount, on
  a 16-node cache.
- **A crash test:** about 2,000 simulated power cuts, at every write of a
  transaction. Unflushed writes are randomly kept, dropped or torn. The
  remounted state must be exactly the old or the new one, and check clean.
- **Damage tests:** broken superblock, flipped bits in data, nodes and
  bitmap.
- **The measurements above.**
