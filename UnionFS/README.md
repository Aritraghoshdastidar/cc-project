# Mini-UnionFS (FUSE3)

A simplified Union File System in userspace with:
- lower layer (read-only)
- upper layer (read-write)
- merged mount point

Implements:
- path resolution with whiteout precedence
- copy-on-write on write-open of lower-only files
- whiteout-based deletion hiding
- basic POSIX operations (`getattr`, `readdir`, `open`, `read`, `write`, `create`, `unlink`, `mkdir`, `rmdir`)

## Build (Ubuntu 22.04)

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libfuse3-dev
make
```

## Run

```bash
./mini_unionfs <lower_dir> <upper_dir> <mount_point>
```

## Test

```bash
chmod +x test_unionfs.sh
./test_unionfs.sh
```
