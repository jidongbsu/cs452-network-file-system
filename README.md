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

If you are interested in NFS and its implementation in general, or want to understand various details of the starter code, the book "NFS Illustrated" written by Brent Callaghan is what you are recommended to read.

## Background

### File Handle vs Inode

As we have learned before, in a typical file system, we use inodes to represent files and directories: each inode is representing either a file, or a directory. Within the file system, each file or directory has a unique inode number. Coming into the network file system situation, using an inode number is not enough for the nfs client to tell the nfs server which file the client is referring to. The following example shows why: Let's say the nfs server has two different file systems, installed on /dev/sda1 and /dev/sda2. And the nfs server mounts these two file systems like this:

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

Because of the above reason, in network file systems, we introduce a new structure, called **file handles**. The Linux kernel defines *struct knfsd_fh* in *include/uapi/linux/nfsd/nfsfh.h* for this purpose.

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

## Starter Code

The starter code looks like this:

```console
[cs452@xyno cs452-network-file-system]$ ls
bmw_main.c  export.c  export.h  Makefile  netns.h  nfsd.h  nfsfh.c  nfsfh.h  nfssvc.c  proc.c  README.md  rfc1813.txt  vfs.c  vfs.h  xdr.c  xdr.h
```

You will be completing xdr.c, and you should not modify any other files.

The starter code already provides you with the code for a kernel module called bmw. To install the module, run make and then sudo insmod bmw.ko; to remove it, run sudo rmmod bmw. Yes, in rmmod, whether or not you specify ko does not matter; but in insmod, you must have that ko.

What this module does is: It starts a kernel thread, which calls the function *nfsd*(), which runs an infinite *for* loop. Inside this *for* loop, it waits for requests (which are sent by clients), and when a request message is received, *nfsd*() calls *svc_process*() to process this request. Each request is represented by a *struct svc_rqst*. *svc_process*() will call the *vs_dispatch* call back function, which is *nfsd_dispatch*(), and this function will call 3 call back functions: 1. *pc_decode()*, 2. *pc_func()*, 3. *pc_encode()*. Here, *pc_func*() is the RPC call the client wants to invoke. The client, in its request message, encodes information about this RPC call, which tells the server which RPC call the client wants to invoke, and arguments to this call. Because the information is encoded in the request message, the server needs to decode the request message first. Take the REMOVE RPC call for example, when the client wants to delete a file, it sends this call to the server. This call comes with two arguments: a file handle representing the (parent) directory from which the (child) file is to be deleted, and the name of the file to be deleted. To obtain these two arguments, the server invokes its *pc_decode()* call back function. After calling its *pc_decode()* function, the server now has these two arguments, and the server will then call the *pc_func()* call back function, which in this REMOVE RPC call example, will be the remove function provided by the server's local file system, and this function will remove the file from the parent directory. Once this REMOVE RPC call is finished, the server needs to inform the client about the result of this call - success or failure? Such results should be encoded in a reply message, which will be sent from the server to the client. To this purpose, the server calls its *pc_encode*() call back function.

**Note**: because the purpose of these requests is to invoke RPC calls, such requests are therefore referred to as call requests.

## Functions You Need to Implement

Here are the prototypes of the functions that you need to implement in xdr.c:

```c
static __be32 * decode_file_handle(__be32 *p, struct svc_fh *fhp);
static __be32 * decode_file_name(__be32 *p, char **namp, unsigned int *lenp);
static __be32 * encode_fattr3(struct svc_rqst *rqstp, __be32 *p, struct svc_fh *fhp, struct kstat *stat);
```

*decode_file_handle*() and *decode_file_name*() are used in the *pc_decode*() call back function; whereas *encode_fattr3*() are used in the *pc_encode*() call back function. Still, let's take the REMOVE RPC call as an example, when a client wants to delete a file, it sends a REMOVE RPC call request to the server.  The request message comes as a stream of data, which complies with the XDR format. And this stream of data is represented by *p*, which is the first parameter of both *decode_file_handle()* and *decode_file_name()*. The server will call this *decode_file_handle*() to decode the request message and obtain the file handle representing the (parent) directory from which the (child) file is to be deleted, and will then call *decode_file_name*() to decode the request message and obtain the name of the file to be deleted.

1. When *decode_file_handle()* is called, the memory space for a *struct svc_fh* is already allocated, and it is pointed to by *fhp*, which is the second parameter of *decode_file_handle*(). As to *p*, it is pointing to the beginning location of the file handle. When implementing *decode_file_handle*(), your job is to store the decoded file handle in *fhp->fh_handle.fh_base*, and then update *p*, so that *p* will be pointing to a location (in the data stream) right past the file handle, which is actually the beginning location of the file name.

2. When *decode_file_name*() is called, the memory space for the file name is already allocated, and it is pointed to by *name*, which is the second parameter of *decode_file_name*(), and the memory space for the length of this file name is also allocated, and it is pointed to by *lenp*, which is the third parameter of *decode_file_name*(). And *p* is pointing to the beginning location of the file name, in the data stream.

3. According to the NFS protocol, when a REMOVE RPC call is finished, the server should send a reply message to the client, and this reply message should include the file attributes of the parent directory. All the attributes are included in a *fattr3* structure, which is defined as following:

```c
struct fattr3 {
         ftype3     type;	/* the file type: a regular file or a directory? */
         uint32     mode;	/* file permission bits */
         uint32     nlink;	/* number of links */
         uint32     uid;	/* file ower's user id */
         uint32     gid;	/* file owner's group id */
         uint64     size;	/* file size in bytes */
         uint64     used;	/* disk space the file actually uses */
         specdata3  rdev;	/* file device information */
         uint64     fsid;	/* file system id * /
         uint64     fileid;	/* file number within file system, i.e., the inode number */
         nfstime3   atime;	/* last access time */
         nfstime3   mtime;	/* last modify time */
         nfstime3   ctime;	/* last attribute change time */
      };
```

Similar to the request message, the reply message also goes as a stream of data. When *encode_fattr3*() is called, *p* is pointing to this stream, and the goal of *encode_fattr3*() is to encode the above fattr3 structure into the memory location which is pointed to by *p*. When implementing *encode_fattr3*(), you can obtain the value of each field of this *fattr3* structure from *fhp* and *stat*, which is the third and the fourth parameter of *encode_fattr3*().

## Implementing decode_file_handle()

## Implementing decode_file_name()

## Implementing encode_fattr3()

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

The first time (and only the first time), run the following commands to enable (and start) the rpc service on the server.

```console
[cs452@xyno ~]$ sudo systemctl enable rpcbind.service
[cs452@xyno ~]$ sudo systemctl start rpcbind.service
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

Run the following commands to load the kernel module and start the NFS service:

```console
[cs452@xyno ~]$ sudo insmod bmw.ko
[cs452@xyno ~]$ sudo systemctl start nfs
```

When all tests are done, run the following to stop the NFS server and unload the nfsd kernel module.

```console
[cs452@xyno nfsd]$ sudo systemctl stop nfs
[cs452@xyno nfsd]$ sudo umount /proc/fs/nfsd 
[cs452@xyno nfsd]$ sudo rmmod bmw 
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

After this, you can test the NFS file system within this /tmp/mnt directory. You are required to test commands including *touch*, *mkdir*, *ls -l*, *rm -f*, *rmdir*. You are suggested to use *vi* to edit files, and then use *cat* to read the file.

When all tests are done, leave this /tmp/mnt and run this *umount* command to unmount the NFS file system.

```console
[cs452@xyno ~]$ sudo umount /tmp/mnt
```

## Submission

Due: 23:59pm, December 15th, 2022. Late submission will not be accepted/graded.

## Project Layout

All files necessary for compilation and testing need to be submitted, this includes source code files, header files, and Makefile. The structure of the submission folder should be the same as what was given to you.

## Grading Rubric (Undergraduate and Graduate)

- [70 pts] Functional requirements (from client's perspective):

  - file creation works (touch). /10
  - file read/write works (edit file and then cat). /20
  - directory creation works (mkdir). /10
  - directory list works (ls -l). /10
  - file deletion works (rm -f). /10
  - directory deletion works when the directory is empty (rmdir). /10

- [10 pts] Compiler warnings:

  - Each compiler warning will result in a 3 point deduction.
  - You are not allowed to suppress warnings.
  - You won't get these points if you didn't implement any of the above functional requirements.

- [10 pts] Module can be installed and removed without crashing the system:

  - You won't get these points if your module doesn't implement any of the above functional requirements.

- [10 pts] Documentation:

  - README.md file (rename this current README file to README.orig and rename the README.template to README.md.)
  - You are required to fill in every section of the README template, missing 1 section will result in a 2-point deduction.
