# STORfs SNode

In STORfs,
the concept of SNodes are inspired by inode (index node) from Unix-style file systems.
The SNode is responsible for storing information about the file or directory of concern.
This includes things such as:

- Modified time
- Parent SNode
- Size of the file
- CRC of the SNode block
- Flags
- File Name

An SNode is exactly 128 bytes,
the rest of the page is reserved for inline file or directory data.
The amount of data that can be stored in a page containing an SNode is:

```math
s = p_s - 128
```

Where $s$ is the size of the data and $p_s$ is the page size.
All other data is stored elsewhere on the file system.
This is done through a concept of page extents,
which point to these other locations on the file system.

## Operations

Three operations are available for an SNode:

- read
- write
- erase

## Extents

SNode extents represent a contiguous area of storage reserved for a file.
Extent information is stored in the following format:

```c
typedef struct {
  storfs_page_t start;
  storfs_page_t count;
} SNodeExtent;
```

Where start represents the beginning page location of the extent,
and count represents the number of contiguous pages after.
A physical example of this in the filesystem might look like the following:

```
start = 100
     |
     |
+-----------------------------------------------------------+
| Block 100 | Block 101 | Block 102 | Block 103 | Block 104 |
+-----------------------------------------------------------+
      |                                              |
      |                                              |
      +-----------------------------------------------
                             |
                             |
                        count = 5
```

Within each SNode there are three layers of extents storage.
These are described in the following sections.

### Direct

These extents exist as an array of `DIRECT_EXTENT_SIZE` elements directly in the SNode data structure.
A file's location is assigned checked here first when performing an operation upon it.
This is the location in which the fastest look up times are available,
O(1).
Keeping files within the direct extents help optimize reading operations performed.

### Single Indirect

The single indirect parameter in the SNode entry points to a page consisting of extent entries.
Indirect pages have the following number of extents:

```math
n = p_s / s_e
```

Where $n$ is the number of extents,
$p_s$ is the page size,
and $s_e$ is the size of an extent entry, `sizeof(SNodeExtent)`.
The cost accessing a single indirect extent is:

- Reading a page
