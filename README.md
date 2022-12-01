# Overview

In this assignment, we will write a Linux kernel module called bmw. This module will serve as a Linux NFS server. You should still use the cs452 VM (username:cs452, password: cs452) which you used for your tesla, lexus, infiniti, toyota, lincoln, and audi, as loading and unloading the kernel module requires the root privilege.

## Learning Objectives

- Learning how network file systems (NFS) work
- Understanding the concept of remote procedure calls (RPC) and how they can be implemented

## Important Notes

You MUST build against the kernel version (3.10.0-1160.el7.x86_64), which is the default version of the kernel installed on the cs452 VM.

## Book References

You are recommended to read these two book chapters:

[Distributed Systems](https://pages.cs.wisc.edu/~remzi/OSTEP/dist-intro.pdf). This chapter explains what Remote Procedure Calls are and what eXternal Data Representation (XDR) is, both concepts are the foundation of this assignment.

[Network File System](https://pages.cs.wisc.edu/~remzi/OSTEP/dist-nfs.pdf). This chapter explains what are the commonly used RPC calls in the NFS protocol.

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
// fh_size indicates the actual size of this file handle, whereas fh_base indicates the actual content of this file handle.
struct knfsd_fh {
        unsigned int    fh_size;        /* significant for NFSv3.
                                         * Points to the current size while building
                                         * a new file handle
                                         */
        union {
                struct nfs_fhbase_old   fh_old; // This is the old "dentry style" Linux NFSv2 file handle.
                __u32                   fh_pad[NFS4_FHSIZE/4];
                struct nfs_fhbase_new   fh_new; // This is the new flexible, extensible style NFSv2/v3 file handle.
        } fh_base;
};
```

Each file handle represents a file or a directory.

# Specification

## Testing

**Note**: in the following, we assume the NFS server's IP address is 192.168.56.114. Replace this ip address with your NFS server's IP address. Before you can test your network file system, make sure there is a network connection between your client and server - for example, they should be able to *ping* each other, and *ssh* to each other.

### NFS Server Side

#### One Time Setup

The first time (and only the first time), run the following commands to disable firewall on the server side - because it by default blocks the NFS service.

```console
[cs452@xyno ~]$ sudo systemctl stop firewalld
[cs452@xyno ~]$ sudo setenforce 0
[cs452@xyno ~]$ sudo systemctl disable firewalld
```

The first time (and only the first time), run this *mkdir* command to create the export directory /opt/test1.

```console
[cs452@xyno ~]$ sudo mkdir /opt/test1
```

And then export this directory in /etc/exports, your /etc/exports should look like this:

```console
[cs452@xyno ~]$ cat /etc/exports
/opt/test1 *(rw,sync,no_root_squash)
```

#### Regular Testing

Run the following commands to load the nfsd kernel module and start the NFS service:

```console
[cs452@xyno ~]$ sudo insmod nfsd.ko
[cs452@xyno ~]$ sudo systemctl start rpcbind.service
[cs452@xyno ~]$ sudo systemctl start nfs
```

When all tests are done, run the following to stop the NFS server and unload the nfsd kernel module.

```console
[cs452@xyno nfsd]$ sudo systemctl stop nfs
[cs452@xyno nfsd]$ sudo umount /proc/fs/nfsd 
[cs452@xyno nfsd]$ sudo rmmod nfsd
```

### NFS Client Side

#### One Time Setup

The first time (and only the first time), run this *mkdir* command to create the mount point directory /tmp/mnt.

```console
[cs452@xyno ~]$ mkdir /tmp/mnt
```

#### Regular Testing

Once the mount point directory is created, run the following command to mount the remote directory exported by the NFS server:

```console
[cs452@xyno ~]$ sudo mount -t nfs 192.168.56.114:/opt/test1 /tmp/mnt
```

If the above *mount* command fails, run this to confirm your server is indeed running and its /opt/test1 directory is indeed exported:

```console
[cs452@xyno ~]$ showmount -e 192.168.56.114
Export list for 192.168.56.114:
/opt/test1 *
```

If your showmount command shows a result like above, then your mount command should succeed; if not, then very likely there is no network connection between your client and server.

Next, run *cd* to enter into this directory.

```console
[cs452@xyno ~]$ cd /tmp/mnt
```

After this, you can test the NFS file system within this /tmp/mnt directory.

When all tests are done, leave this /tmp/mnt and run this *umount* command to unmount the NFS file system.

```console
[cs452@xyno ~]$ sudo umount /tmp/mnt
```

## Submission

Due: 23:59pm, December 15th, 2022. Late submission will not be accepted/graded.

## Project Layout

## Grading Rubric (Undergraduate and Graduate)
