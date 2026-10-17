# SieFS: the SIEOS file system

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

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

(A grown volume has its bitmap copies elsewhere, among the data: see
Growing.)

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
| `EXTENT` 3 | byte offset in the file | first disk block, block count (≤ 32), flags, one CRC per block, then (flag `X_GEN`) the commit that wrote the blocks |
| `DIRENT` 4 | 56-bit hash of the name | list of `ino (8) · type (1) · name length (1) · name` |
| `XATTR` 5 | 56-bit hash of the name | list of `name length (1) · value length (2) · name · value` |
| `XINDEX` 6 | object id | none; the id is `1 << 63 \| hash(name, value)` |
| `VOLUME` 16 | 0 | in the root tree: the volume's file tree, next id, object count, flags, key id, and for snapshots their head and commit, dead list, creation time |
| `VOLNAME` 17 | 0 | in the root tree: a snapshot's name |
| `DEAD` 18 | first block | in the root tree, keyed by a dead list's id: a run of blocks a snapshot still uses (count, commit that wrote them) |
| `VOLKEY` 19 | 0 | in the root tree: an encrypted volume's key, sealed by the key-encryption key; how that is derived; the owner, mode and mount point its locked root shows |

Objects (files, directories, symlinks) have ids that are never reused;
the root directory is object 2. Names that hash alike share one item.

### Two levels of trees: volumes

The superblock points at a **root tree** whose items are **volumes**,
each with its own **file tree**. Volume 1 is "/". Another volume is
mounted at a directory entry of its own (`siefs_mkvol`), for example a
user's home: the entry holds the volume's root, and that root's parent
is the entry's directory. An object's number says which volume it is in:
`volume << 48 | id`, and just the id in volume 1, so disks made before
volumes existed read the same. Rename and hard links do not cross
volumes (`EXDEV`); a mounted volume's entry cannot be removed or renamed
(`EBUSY`); `siefs_rmvol` removes an empty one.

## Snapshots

A **snapshot** is a read-only picture of a whole volume at one commit. It
is a volume record whose tree root is the one its volume (its *head*) had
then: nothing is copied, so taking one costs a few blocks, at any size.
Each volume's root has a hidden `.snapshots` directory (it is not stored:
the library makes it up) listing them by name, so `/home/alice/.snapshots/
before-upgrade/notes.txt` is that file as it was. Writing there is
`EROFS`. `siefs_snapshot_restore` makes the head what a snapshot shows
(newer snapshots are deleted first).

**Keeping what a snapshot uses.** After a snapshot, the head and the
snapshot share blocks; the head keeps copying on write, and must not free
a block the snapshot still needs. Every block knows the commit that wrote
it (a node: its pointer's `gen`; data: its extent's `gen`), and a head
knows its newest snapshot's commit. When the head lets go of a block:

- written after that commit: it was the head's alone, it is freed as
  usual;
- written at or before it: the snapshot uses it, so it goes on the head's
  **dead list** instead (items `(dead list, DEAD, first block)` in the root
  tree), still marked in use.

When a snapshot is taken, the head's dead list becomes the snapshot's,
and the head starts a new one. Each dead list thus holds the blocks freed
between the snapshot before it and itself. **Deleting** snapshot S, with P
the snapshot before it and N the one after it (or the head): N's dead list
holds the blocks freed between S and N. Those written after P were used
only by S: they are freed. The others P still uses: they join S's dead
list, which becomes N's. No tree is walked; only that dead list is read.
This is how ZFS deletes snapshots. **Restoring** S walks the head's tree
down only where blocks are newer than S, frees them, drops the head's dead
list (S uses those blocks again), and gives the head S's root.

Extents written before `X_GEN` existed count as written at commit 0:
older than any snapshot, which is exactly what the rule needs.

Each of these operations is one commit, like everything else. The tests
take, delete and restore snapshots at random while comparing every
snapshot with a copy of the model taken with it, and cut the power during
them (see Tests).

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

## Growing

`siefs_grow(fs, nblocks)` makes a mounted volume bigger, up to the
device's size (`env.nblocks`): SIEOS's images are small and grow to fill
the USB key or SD card they are written to ([usb.md](usb.md#small-image-grown-on-the-first-start)).
The file server calls it at mount when the disk is bigger than the volume;
the host tool has `siefs IMAGE grow [SIZE]`.

The bitmap copies are sized for the old volume (one block per 128 MiB), so
growing moves them: both copies and their indexes go to the start of the
new space, the new blocks become free, and the old copies are freed like
any block (usable after the next commit: the previous superblock still
points at them). It is **one commit**, written in the usual order: the new
copy of this slot's bitmap (whole), its index, then the superblock with
the new size and the new places. A power cut before that superblock lands
leaves the old volume, whole (nothing it uses was touched); after, the
grown one; the next commit writes the other slot's copy whole at its new
place. After a grow, `data_start` is 3: the bitmap copies are among the
data, where the superblock says (`bm[]`, `idx[]`; checked at mount to lie
inside the volume). An older SieFS refuses to mount a grown volume
rather than misread it. Growing again moves the copies again. Shrinking is
not supported.

The cost: ~170 lines of code all told (the grow ~40 in `space.c`, the
checker's 4, the GPT and MBR part in the disk server). `make siefs-test`
grows a volume twice (600 -> 34,000 -> 70,000 blocks: 1, 2, then 3
bitmap blocks a copy) with a power cut at every write of each grow (and
the unflushed writes kept, dropped or torn): the volume always comes back
old or grown, with the same files, checks clean, and grows after; then
files fill the new space and read back.

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

## Encryption

A volume can be **encrypted** (`siefs_mkvol_enc`): SIEOS makes each
user's home one, keyed by their password.

- **Keys:** the volume key is 32 random bytes. It is kept in the root
  tree (`VOLKEY` item), sealed with a key-encryption key that the caller
  derives from the password with Argon2id (the item records how: salt,
  passes, memory). Changing the password seals the same key anew
  (`siefs_rekey`): nothing else is rewritten.
- **Cipher:** ChaCha20-Poly1305 (`lib/aead.c`: fast without special CPU
  instructions), authenticated: tampering, even copying old blocks back,
  is detected, not just garbled.
- **What is sealed:**
  - every tree node, except its 32-byte header (level, item count, commit,
    tree), which is authenticated as associated data; its tag goes in the
    pointer to it (`bptr_t.mac`), the nonce's salt in the pointer's flags;
  - every data block, with the volume id as associated data; its tag in
    the extent (one per block), the salt once per extent (`X_ENC`).
  So names, attributes, small files kept inside the tree and file contents
  are all sealed.
- **Nonces:** the block's number and a 4-byte salt, random per commit for
  nodes and a counter from a random start for data writes (the top bit
  tells them apart). A block is never sealed twice with the same nonce in
  one commit; a commit redone after a crash draws a new salt.
- **Checksums** stay those of what is on the disk, so an encrypted volume
  checks without its key (but its free space is not compared: see below).
- **Locked:** after a mount, until `siefs_unlock` with the right key.
  Its root still stats, as its owner's directory (the `VOLKEY` item keeps
  owner, mode and mount point); everything else in it is `EACCES`.
  `siefs_lock` closes it again (its cached nodes are dropped). Its
  snapshots share its key.
- **What stays visible:** the root tree (which volumes exist, their sizes
  in objects), the bitmap (which blocks are used), node headers (how many
  items, which commit), and how big files are on the disk.
- **Checking** a locked volume: its blocks cannot be walked, so
  `siefs_check` skips it and does not look for leaks (it says so).

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
- **Not yet:** quotas.
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
| `siefs/fs.c` | files, directories, links, rename, attributes; volumes: picking one per operation, mounting |
| `siefs/vol.c` | snapshots: taking, deleting, restoring, listing |
| `siefs/crypt.c` | encrypted volumes: sealing and opening nodes and blocks, the volume key |
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
siefs disk.img grow 8G                              make the image 8 GiB and the volume with it
```

## Tests

`make siefs-test` runs:
- **Unit tests** of every operation and its errors.
- **A model test:** 12,000 random operations compared against an
  in-memory model, with a full check after every commit and remount, on
  a 16-node cache.
- **A snapshot test:** the model test again, inside a mounted volume,
  while snapshots are taken, deleted and restored at random (about 80, 70
  and 10 a run): every snapshot must keep showing the model as it was when
  taken, and everything must check clean (dead lists included, no leaks)
  after every commit. At the end, with the files, snapshots and volume
  removed, all the space is free again. `siefs-test snapshots SEED` runs
  it with another seed.
- **An encryption test:** nothing of an encrypted volume in clear on the
  disk (contents, names, small files, attributes), locked after a mount,
  the wrong key refused, a new password, a changed block refused by its
  tag. The snapshot test also runs inside an encrypted volume.
- **A grow test:** see Growing.
- **A crash test:** about 6,000 simulated power cuts, at every write of a
  transaction, half the scenarios with snapshots taken, deleted and
  restored, a third inside an encrypted volume. Unflushed writes are randomly kept, dropped or torn. The
  remounted state must be exactly the old or the new one (with snapshots,
  which commit by themselves: the state after one of the operations), and
  check clean.
- **Damage tests:** broken superblock, flipped bits in data, nodes and
  bitmap.
- **The measurements above.**
