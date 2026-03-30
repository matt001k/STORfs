# STORfs SNode

In STORfs,
SNodes are inspired by the inode (index node) from Unix-style file systems.
The SNode is responsible for storing information about the file or directory of concern.
This includes:

- Last modified time
- Parent SNode
- Size of the data held in the SNode
- CRC of the SNode structure
- Flags
- File Name

SNodes are saved to pages in the filesystem and are exactly 128 bytes.
The rest of the page it is saved to is reserved for inline file or directory data.
The following calculates the amount of inline data following the SNode:

```math
s = p_s - 128
```

Where $s$ is the size of the data and $p_s$ is the filesystem's page size.
All other data for the associated SNode is stored elsewhere on the file system.
This is done through a concept of page extents,
which point to the other locations on the file system.

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
A visual example of this in the filesystem with `start = 100` and `count = 5` would look like:

```
start = 100
     |
     |
+------------------------------------------------------+
| Page 100 | Page 101 | Page 102 | Page 103 | Page 104 |
+------------------------------------------------------+
      |                                           |
      |                                           |
      +-------------------------------------------+
                             |
                             |
                        count = 5
```

The extent starts on page **100** and extends for **5** pages to page **104**.

Within each SNode there are three layers of extents storage.
These are described in the subsequent sections.

### Direct

These extents exist as an array of `DIRECT_EXTENT_SIZE` elements directly in the SNode data structure.
When finding a file's location,
the direct extents are checked first.
This is the location in which the fastest look up times are available.
Keeping files within the direct extents can greatly speed up initialy accessing an SNode (this process will be explained in SNode [operations](#operations)).

### Single Indirect

The single indirect parameter in the SNode entry points to a page consisting fully of extent entries.
This page of extents can be visually represented as follows:

```
+----------+
| Extent 1 |
+----------+
| Extent 2 |
+----------+
| Extent 3 |
+----------+
| Extent 4 |
+----------+
| Extent 5 |
+----------+
| Extent 6 |
+----------+
| Extent 7 |
+----------+
|   ...    |
| Extent N |
+----------+
```

Indirect pages have the following number of extents:

```math
N = p_s / s_e
```

Where $N$ is the number of extents,
$p_s$ is the page size,
and $s_e$ is the size of an extent entry, `sizeof(SNodeExtent)`.

### Multiple Extents

The final layer to the extents are the multiple extents.
Each individual


## Operations

Four operations are available for an SNode:

- locate
- read
- write
- erase
