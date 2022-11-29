# Overview

In this assignment, we will write a Linux kernel module called bmw. This module will serve as a Linux NFS server. You should still use the cs452 VM (username:cs452, password: cs452) which you used for your tesla, lexus, infiniti, toyota, lincoln, and audi, as loading and unloading the kernel module requires the root privilege.

## Learning Objectives

## Important Notes

## Book References

## Background

### File Handle vs Inode

As we have learned before, in a typical file system, we use inodes to represent files and directories: each inode is represent either a file, or a directory. Within the file system, each file or directory has a unique inode number. Coming into the network file system situation, using an inode number is not enough for the nfs client to tell the nfs server which file the client is referring to. The following example shows why: Let's say the nfs server two different file systems, installed on /dev/sda1 and /dev/sda2. And the nfs server mounts these two file systems like this:

```console
# sudo mount /dev/sda1 /opt/test1
# sudo mount /dev/sda2 /opt/test1/test2
```

And both of these two directories are exported to the nfs client:

```console
/opt/test1  *(rw,sync,no_root_squash)
/opt/test1/test2  *(nohide,rw,sync,no_root_squash)
```

Now if the nfs client mounts the first file system like this:

```console
# sudo mount -t nfs nfs_server:/opt/test1  /tmp/mnt
```

Interestingly, this one single command actually mounts the two file systems on the client side. And then if the client is accessing a file inside /tmp/mnt/test2, and passing an inode number (which is associated to this file) to the server, how does the server know which file system the client is actually accessing? It is possible that an inode number, such as 10, is used by the first file system to represent the file /opt/test1/A, but is also used by the second file system to represent the file /opt/test1/test2/B. Therefore, if the server receives inode number 10, it will get confused: does the client want to access file A, or file B?

Because of the above reason, in network file systems, we do not just use inodes, we use a new structure, called **file handles**. The Linux kernel defines *struct knfsd_fh** in *include/uapi/linux/nfsd/nfsfh.h* for this purpose.

```c
struct knfsd_fh {
        unsigned int    fh_size;        /* significant for NFSv3.
                                         * Points to the current size while building
                                         * a new file handle
                                         */
        union {
                struct nfs_fhbase_old   fh_old;
                __u32                   fh_pad[NFS4_FHSIZE/4];
                struct nfs_fhbase_new   fh_new;
        } fh_base;
};
```

# Specification

## Testing

## Submission

Due: 23:59pm, December 15th, 2022. Late submission will not be accepted/graded.

## Project Layout

## Grading Rubric (Undergraduate and Graduate)
