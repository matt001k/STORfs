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
This is done through a concept called page extents,
which point to the other data locations on the file system.

## Extents

SNode extents represent a contiguous area of storage reserved for SNode data.
Extent information is stored in the following format:

```c
typedef struct {
  storfs_page_t start;
  storfs_page_t count;
} SNodeExtent;
```

Where start represents the beginning page location of the extent,
and count represents the total number of contiguous pages in the extent.
A visual example of this with `start = 100` and `count = 5` would look like:

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

Within each SNode there are three layers of extent storage.
These are described in the subsequent sections.

### Direct

These extents exist as an array of `DIRECT_EXTENT_SIZE` elements directly in the SNode data structure.
When finding an SNode's data location,
the direct extents are checked first.
This is the location in which the fastest lookup times are available: **O(1)**.
Keeping files within the direct extents can greatly speed up initially accessing an SNode (this process will be explained in SNode [operations](#operations)).

### Single Indirect

The single indirect parameter in the SNode entry points to a page consisting fully of extent entries.
A single indirect extent page can be visually represented as follows:

```
+----------+
| Extent 1 | ---> start = 100, count = 5
+----------+
| Extent 2 | ---> start = 140, count = 12
+----------+
| Extent 3 | ---> start = 409, count = 2
+----------+
| Extent 4 | ---> start = 1030, count = 136
+----------+
| Extent 5 | ---> start = 1117, count = 23
+----------+
| Extent 6 | ---> start = 2020, count = 1
+----------+
| Extent 7 | ---> start = 2023, count = 30
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

The final layer of extents are multiple extents.
Each entry in the multiple extent pages has the following contents:

```c
typedef struct {
  storfs_page_t single_location;
  storfs_page_t total;
} SNodeMultiple;
```

`single_location` contains the location of a single indirect extent page
and `total` indicates the absolute number of pages allocated in that indirect page.
`total` can be calculated as:

```math
t = \sum_{i=1}^{N} c_i
```

Where $t$ is the total number of pages allocated in the single indirect extent page.
$N$ is the number of extents in the single indirect extent page.
$c_i$ is the count of the $i$th extent in the single indirect extent page.

A multiple extent page can be visually represented as follows:

```
+------------+
| Multiple 1 | --->  +----------+
+------------+       | Extent 1 | ---> start = 100, count = 5
                     +----------+
                     | Extent 2 | ---> start = 140, count = 12
                     +----------+
                     | Extent 3 | ---> start = 409, count = 2
                     +----------+
                     | Extent 4 | ---> start = 1030, count = 136
                     +----------+
                     | Extent 5 | ---> start = 1117, count = 23
                     +----------+
                     | Extent 6 | ---> start = 2020, count = 1
                     +----------+
                     | Extent 7 | ---> start = 2023, count = 30
                     +----------+
```

In the above example,
`total` of `Multiple 1` would be `209`.

The number of multiple entries in a page can be calculated as:

```math
N = p_s / s_e
```

Where $N$ is the number of multiple extent entries,
$p_s$ is the page size,
and $s_e$ is the size of a multiple entry, `sizeof(SNodeMultiple)`.

The total number of extent entries,
`SNodeExtent`,
which can be held in the multiple extent page can be calculated as:


```math
N = N_m * N_s
```

Where $N$ is the total number of extent entries,
$N_m$ is the number of entries in the multiple extent page,
and $N_s$ is the number of entries in a single extent page.


## Operations

Five operations are available on an SNode after it has been created:

| Operation | Complexity |
|-----------|------------|
| Lookup    | O(1)       |
| Locate    | O(n)       |
| Read      | O(1)       |
| Write     | O(1)       |
| Erase     | O(1)       |

Where *n* is the number of extent entries traversed to find the target position.

### Lookup

Function declaration:

`snode_lookup`

If the SNode has been created,
it must be looked up before it can be used.
The lookup functionality finds the SNode by page number,
which must be found by another means before calling this API.

### Locate

Function declarations:

- `snode_find_read_location`
- `snode_find_write_location`

The locate operation must be done before performing a read or write/erase.

If an SNode is opened to be read,
the find read location function must be invoked.
This function takes an offset parameter to allow reading from a desired location in the SNode.

If an SNode is opened to be written to or data is to be erased,
the find write location must be invoked.
This function finds the end of the SNode's data,
due to the following reasons:

- All write operations append to an SNode's data
- All erase operations erase from the end of an SNode

All locate functionality has an **O(n)** time complexity,
where *n* is the number of extent entries traversed.
This has the lowest performance of the operations performed on an SNode,
but it only needs to be run once on an SNode.

### Read

Function declaration:

`snode_read_data`

Data is read starting from the locate position.
As data is read,
a data pointer is used to keep track of the location in the SNode's data.
This means that the SNode's data can be read in chunks.
Data is read from the SNode and stored in a buffer passed in from the caller.
All read functionality has an **O(1)** time complexity.

### Write

Function declaration:

`snode_write_data`

Data is always written to the end of an SNode.
As data is written,
an SNode data pointer is used to keep track of where the end of the SNode is.
All write functionality has an **O(1)** time complexity.

### Erase

Function declaration:

`snode_erase_data`

Data is always erased from the end of an SNode.
As data is erased,
an SNode data pointer is used to keep track of where the end of the SNode data is.
Write and erase operations can be used in conjunction with one another.
All erase functionality has an **O(1)** time complexity.
