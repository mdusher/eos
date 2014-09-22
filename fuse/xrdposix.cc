//------------------------------------------------------------------------------
// File: xrdposix.cc
// Author: Elvin-Alin Sindrilaru / Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/************************************************************************/
/* Based on 'xrdposix.cc' XRootD Software                               */
/* xrdposix.cc                                                          */
/*                                                                      */
/* Author: Wei Yang (Stanford Linear Accelerator Center, 2007)          */
/*                                                                      */
/* C wrapper to some of the Xrootd Posix library functions              */
/*                                                                      */
/* Modified: Andreas-Joachim Peters (CERN,2008) XCFS                    */
/* Modified: Andreas-Joachim Peters (CERN,2010) EOS                     */
/************************************************************************/

#define _FILE_OFFSET_BITS 64

/*----------------------------------------------------------------------------*/
#include "xrdposix.hh"
#include "FuseCacheEntry.hh"
#include "fst/layout/LayoutPlugin.hh"
#include "fst/layout/PlainLayout.hh"
#include "fst/layout/RaidDpLayout.hh"
#include "fst/layout/ReedSLayout.hh"
/*----------------------------------------------------------------------------*/
#include <climits>
#include <stdint.h>
#include <iostream>
#include <libgen.h>
#include <pwd.h>
#include <string.h>
#include <pthread.h>
/*----------------------------------------------------------------------------*/
#include "FuseCache/FuseWriteCache.hh"
#include "FuseCache/FileAbstraction.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucHash.hh"
#include "XrdOuc/XrdOucTable.hh"
#include "XrdOuc/XrdOucString.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSfs/XrdSfsInterface.hh"
/*----------------------------------------------------------------------------*/
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
/*----------------------------------------------------------------------------*/
#include "common/Logging.hh"
#include "common/Path.hh"
#include "common/RWMutex.hh"
/*----------------------------------------------------------------------------*/
#include <google/dense_hash_map>
#include <google/sparse_hash_map>
#include <google/sparsehash/densehashtable.h>
/*----------------------------------------------------------------------------*/

#ifndef __macos__
#define OSPAGESIZE 4096
#else
#define OSPAGESIZE 65536
#endif

static FuseWriteCache* XFC;

static XrdOucHash<XrdOucString>* passwdstore;
static XrdOucHash<XrdOucString>* stringstore;

XrdSysMutex passwdstoremutex;
XrdSysMutex stringstoremutex;
XrdSysMutex environmentmutex;
XrdSysMutex connectionIdMutex;
int connectionId = 0;

int fuse_cache_read;
int fuse_cache_write;

bool fuse_exec = false; // indicates if files should be make exectuble

bool fuse_shared = false; // inidicated if this is eosd = true or eosfsd = false

XrdOucString MgmHost; // host name of our FUSE contact point

using eos::common::LayoutId;

//------------------------------------------------------------------------------
// String store
//------------------------------------------------------------------------------

char*
STRINGSTORE (const char* __charptr__)
{
  XrdOucString* yourstring;

  if (!__charptr__) return (char*) "";

  if ((yourstring = stringstore->Find(__charptr__)))
  {
    return ( (char*) yourstring->c_str());
  }
  else
  {
    XrdOucString* newstring = new XrdOucString(__charptr__);
    stringstoremutex.Lock();
    stringstore->Add(__charptr__, newstring);
    stringstoremutex.UnLock();
    return (char*) newstring->c_str();
  }
}



//------------------------------------------------------------------------------
//             ******* Implementation Translations *******
//------------------------------------------------------------------------------

// Protecting the path/inode translation table
eos::common::RWMutex mutex_inode_path;

// Mapping path name to inode
google::dense_hash_map<std::string, unsigned long long> path2inode;

// Mapping inode to path name
google::dense_hash_map<unsigned long long, std::string> inode2path;

//------------------------------------------------------------------------------
// Lock read
//------------------------------------------------------------------------------

void
xrd_lock_r_p2i ()
{
  mutex_inode_path.LockRead();
}


//------------------------------------------------------------------------------
// Unlock read
//------------------------------------------------------------------------------

void
xrd_unlock_r_p2i ()
{
  mutex_inode_path.UnLockRead();
}


//------------------------------------------------------------------------------
// Drop the basename and return only the last level path name
//------------------------------------------------------------------------------

char*
xrd_basename (unsigned long long inode)
{
  eos::common::RWMutexReadLock vLock(mutex_inode_path);
  const char* fname = xrd_path(inode);

  if (fname)
  {
    std::string spath = fname;
    size_t len = spath.length();

    if (len)
    {
      if (spath[len - 1] == '/')
      {
        spath.erase(len - 1);
      }
    }

    size_t spos = spath.rfind("/");

    if (spos != std::string::npos)
    {
      spath.erase(0, spos + 1);
    }

    return static_cast<char*> (STRINGSTORE(spath.c_str()));
  }

  return 0;
}

//----------------------------------------------------------------------------
//! Return the CGI of an URL
//----------------------------------------------------------------------------

const char*
get_cgi (const char* url)
{
  return url ? (strchr(url, '?')) : 0;
}

//----------------------------------------------------------------------------
//! Return the CGI of an URL
//----------------------------------------------------------------------------

XrdOucString
get_url_nocgi (const char* url)
{
  XrdOucString surl = url;
  surl.erase(surl.find("?"));
  return surl;
}

//------------------------------------------------------------------------------
// Translate from inode to path
//------------------------------------------------------------------------------

const char*
xrd_path (unsigned long long inode)
{
  // Obs: use xrd_lock_r_p2i/xrd_unlock_r_p2i in the scope of the returned string
  if (inode2path.count(inode))
    return inode2path[inode].c_str();
  else
    return 0;
}


//------------------------------------------------------------------------------
// Translate from path to inode
//------------------------------------------------------------------------------

unsigned long long
xrd_inode (const char* path)
{
  eos::common::RWMutexReadLock rd_lock(mutex_inode_path);
  unsigned long long ret = 0;

  if (path2inode.count(path))
    ret = path2inode[path];

  return ret;
}


//------------------------------------------------------------------------------
// Store an inode <-> path mapping
//------------------------------------------------------------------------------

void
xrd_store_p2i (unsigned long long inode, const char* path)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_inode_path);
  path2inode[path] = inode;
  inode2path[inode] = path;
}


//------------------------------------------------------------------------------
// Store an inode <-> path mapping given the parent inode
//------------------------------------------------------------------------------

void
xrd_store_child_p2i (unsigned long long inode,
                     unsigned long long childinode,
                     const char* name)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_inode_path);
  std::string fullpath = inode2path[inode];
  std::string sname = name;

  eos_static_debug("parent_inode=%llu, child_inode=%llu, name=%s, fullpath=%s",
                   inode, childinode, name, fullpath.c_str());

  if (sname != ".")
  {
    // we don't need to store this one
    if (sname == "..")
    {
      if (inode == 1)
      {
        fullpath = "/";
      }
      else
      {
        size_t spos = fullpath.find("/");
        size_t bpos = fullpath.rfind("/");

        if ((spos != std::string::npos) && (spos != bpos))
        {
          fullpath.erase(bpos);
        }
      }
    }
    else
    {
      fullpath += "/";
      size_t spos = fullpath.find("//");

      while (spos != std::string::npos)
      {
        fullpath.replace(spos, 2, "/");
        spos = fullpath.find("//");
      }

      fullpath += name;
    }

    eos_static_debug("sname=%s fullpath=%s inode=%llu childinode=%llu ",
                     sname.c_str(), fullpath.c_str(), inode, childinode);
    path2inode[fullpath] = childinode;
    inode2path[childinode] = fullpath;
  }
}


//------------------------------------------------------------------------------
// Delete an inode <-> path mapping given the inode
//------------------------------------------------------------------------------

void
xrd_forget_p2i (unsigned long long inode)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_inode_path);

  if (inode2path.count(inode))
  {
    std::string path = inode2path[inode];
    path2inode.erase(path);
    inode2path.erase(inode);
  }
}



//------------------------------------------------------------------------------
//      ******* Implementation of the directory listing table *******
//------------------------------------------------------------------------------

// Protecting the directory listing table
eos::common::RWMutex mutex_dir2inodelist;

// Dir listing map
google::dense_hash_map<unsigned long long, std::vector<unsigned long long> > dir2inodelist;
google::dense_hash_map<unsigned long long, struct dirbuf> dir2dirbuf;


//------------------------------------------------------------------------------
// Lock read
//------------------------------------------------------------------------------

void
xrd_lock_r_dirview ()
{
  mutex_dir2inodelist.LockRead();
}

//------------------------------------------------------------------------------
// Unlock read
//------------------------------------------------------------------------------

void
xrd_unlock_r_dirview ()
{
  mutex_dir2inodelist.UnLockRead();
}


//------------------------------------------------------------------------------
// Lock write
//------------------------------------------------------------------------------

void
xrd_lock_w_dirview ()
{
  mutex_dir2inodelist.LockWrite();
}


//------------------------------------------------------------------------------
// Unlock write
//------------------------------------------------------------------------------

void
xrd_unlock_w_dirview ()
{
  mutex_dir2inodelist.UnLockWrite();
}


//------------------------------------------------------------------------------
// Create a new entry in the maps for the current inode (directory)
//------------------------------------------------------------------------------

void
xrd_dirview_create (unsigned long long inode)
{
  eos_static_debug("inode=%llu", inode);
  //Obs: path should be attached beforehand into path translation
  eos::common::RWMutexWriteLock vLock(mutex_dir2inodelist);
  dir2inodelist[inode].clear();
  dir2dirbuf[inode].p = 0;
  dir2dirbuf[inode].size = 0;
}


//------------------------------------------------------------------------------
// Delete entry from maps for current inode (directory)
//------------------------------------------------------------------------------

void
xrd_dirview_delete (unsigned long long inode)
{
  eos_static_debug("inode=%llu", inode);
  eos::common::RWMutexWriteLock wr_lock(mutex_dir2inodelist);

  if (dir2inodelist.count(inode))
  {
    if (dir2dirbuf[inode].p)
    {
      free(dir2dirbuf[inode].p);
    }

    dir2dirbuf.erase(inode);
    dir2inodelist[inode].clear();
    dir2inodelist.erase(inode);
  }
}


//------------------------------------------------------------------------------
// Get entry's inode with index 'index' from directory
//------------------------------------------------------------------------------
unsigned long long
xrd_dirview_entry (unsigned long long dirinode,
                   size_t index,
                   int get_lock)
{
  eos_static_debug("dirinode=%llu, index=%zu", dirinode, index);

  if (get_lock) eos::common::RWMutexReadLock rd_lock(mutex_dir2inodelist);

  if ((dir2inodelist.count(dirinode)) &&
      (dir2inodelist[dirinode].size() > index))
    return dir2inodelist[dirinode][index];

  return 0;
}


//------------------------------------------------------------------------------
// Get dirbuf corresponding to inode
//------------------------------------------------------------------------------
struct dirbuf*
xrd_dirview_getbuffer (unsigned long long inode, int get_lock)
{
  if (get_lock) eos::common::RWMutexReadLock rd_lock(mutex_dir2inodelist);

  if (dir2dirbuf.count(inode))
    return &dir2dirbuf[inode];
  else
    return 0;
}


//------------------------------------------------------------------------------
//      ******* Implementation of the FUSE directory cache *******
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Get maximum number of directories in cache
//------------------------------------------------------------------------------

static const unsigned long long
GetMaxCacheSize ()
{
  return 1024;
}

// Protecting the cache entry map
eos::common::RWMutex mutex_fuse_cache;

// Directory cache
google::dense_hash_map<unsigned long long, FuseCacheEntry*> inode2cache;


//------------------------------------------------------------------------------
// Get a cached directory
//------------------------------------------------------------------------------
int
xrd_dir_cache_get (unsigned long long inode,
                   struct timespec mtime,
                   struct dirbuf** b)
{
  int retc = 0;
  FuseCacheEntry* dir = 0;
  eos::common::RWMutexReadLock rd_lock(mutex_fuse_cache);

  if (inode2cache.count(inode) && (dir = inode2cache[inode]))
  {
    struct timespec oldtime = dir->GetModifTime();

    if ((oldtime.tv_sec == mtime.tv_sec) &&
        (oldtime.tv_nsec == mtime.tv_nsec))
    {
      // Dir in cache and valid
      *b = static_cast<struct dirbuf*> (calloc(1, sizeof ( dirbuf)));
      dir->GetDirbuf(*b);
      retc = 1; // found
    }
  }

  return retc;
}


//------------------------------------------------------------------------------
// Forget a cached directory
//------------------------------------------------------------------------------
int
xrd_dir_cache_forget (unsigned long long inode)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_fuse_cache);

  if (inode2cache.count(inode))
  {
    inode2cache.erase(inode);
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
// Add or update a cache directory entry
//------------------------------------------------------------------------------
void
xrd_dir_cache_sync (unsigned long long inode,
                    int nentries,
                    struct timespec mtime,
                    struct dirbuf* b)
{
  eos::common::RWMutexWriteLock wr_lock(mutex_fuse_cache);
  FuseCacheEntry* dir = 0;

  if ((inode2cache.count(inode)) && (dir = inode2cache[inode]))
  {
    dir->Update(nentries, mtime, b);
  }
  else
  {
    // Add new entry
    if (inode2cache.size() >= GetMaxCacheSize())
    {
      // Size control of the cache
      unsigned long long indx = 0;
      unsigned long long entries_del =
        static_cast<unsigned long long> (0.25 * GetMaxCacheSize());
      google::dense_hash_map<unsigned long long, FuseCacheEntry*>::iterator iter;
      iter = inode2cache.begin();

      while ((indx <= entries_del) && (iter != inode2cache.end()))
      {
        dir = (FuseCacheEntry*) iter->second;
        inode2cache.erase(iter++);
        delete dir;
        indx++;
      }
    }

    dir = new FuseCacheEntry(nentries, mtime, b);
    inode2cache[inode] = dir;
  }

  return;
}


//------------------------------------------------------------------------------
// Get a subentry from a cached directory
//------------------------------------------------------------------------------
int
xrd_dir_cache_get_entry (fuse_req_t req,
                         unsigned long long inode,
                         unsigned long long entry_inode,
                         const char* efullpath)
{
  int retc = 0;
  eos::common::RWMutexReadLock rd_lock(mutex_fuse_cache);
  FuseCacheEntry* dir;

  if ((inode2cache.count(inode)) && (dir = inode2cache[inode]))
  {
    if (dir->IsFilled())
    {
      struct fuse_entry_param e;
      if (dir->GetEntry(entry_inode, e))
      {
        xrd_store_p2i(entry_inode, efullpath);
        fuse_reply_entry(req, &e);
        retc = 1; // found
      }
    }
  }

  return retc;
}


//------------------------------------------------------------------------------
// Add new subentry to a cached directory
//------------------------------------------------------------------------------
void
xrd_dir_cache_add_entry (unsigned long long inode,
                         unsigned long long entry_inode,
                         struct fuse_entry_param* e)
{
  eos::common::RWMutexReadLock rd_lock(mutex_fuse_cache);
  FuseCacheEntry* dir = 0;

  if ((inode2cache.count(inode)) && (dir = inode2cache[inode]))
    dir->AddEntry(entry_inode, e);
}



//------------------------------------------------------------------------------
//      ******* Implementation of the open File Descriptor map *******
//------------------------------------------------------------------------------

// Map used for associating file descriptors with XrdCl::File objects
eos::common::RWMutex rwmutex_fd2fabst;
google::dense_hash_map<int, FileAbstraction*> fd2fabst;

// Map <inode, user> to a file descriptor - used only in the xrd_stat method
google::dense_hash_map<std::string, int> inodeuser2fd;

// Pool of available file descriptors
int base_fd = 1;
std::queue<int> pool_fd;


//------------------------------------------------------------------------------
// Create artificial file descriptor
//------------------------------------------------------------------------------
int
xrd_generate_fd ()
{
  int retc = -1;

  if (!pool_fd.empty())
  {
    retc = pool_fd.front();
    pool_fd.pop();
  }
  else if (base_fd < INT_MAX)
  {
    base_fd++;
    retc = base_fd;
  }
  else
  {
    eos_static_err("error=no more file descirptors available.");
    retc = -1;
  }

  return retc;
}


//------------------------------------------------------------------------------
// Add new mapping between fd and raw file object
//------------------------------------------------------------------------------
int
xrd_add_fd2file (eos::fst::Layout* raw_file,
                 unsigned long inode,
                 uid_t uid)
{
  eos_static_debug("file raw ptr=%p, inode=%lu, uid=%lu",
                   raw_file, inode, (unsigned long) uid);
  int fd = -1;
  std::ostringstream sstr;
  sstr << inode << ":" << (unsigned long)uid;

  eos::common::RWMutexWriteLock wr_lock(rwmutex_fd2fabst);
  auto iter_fd = inodeuser2fd.find(sstr.str());

  // If there is already an entry for the current user and the current inode
  // then we return the old fd
  if (!raw_file)
  {
    if (iter_fd != inodeuser2fd.end())
    {
      eos_static_warning("inodeuid mapping exists, just return old fd=%i",
                         iter_fd->second);
      fd = iter_fd->second;
      auto iter_file = fd2fabst.find(fd);

      if (iter_file != fd2fabst.end())
        iter_file->second->IncNumOpen();
      else
        eos_static_err("fd=%i not found in fd2fobj map", fd);
    }
  }
  else
  {
    fd = xrd_generate_fd();

    if (fd > 0)
    {
      FileAbstraction* fabst = new FileAbstraction(fd, raw_file);
      fd2fabst[fd] = fabst;
      inodeuser2fd[sstr.str()] = fd;
    }
    else
    {
      eos_static_err("error=error while getting file descriptor");
      delete raw_file;
    }
  }

  return fd;
}


//------------------------------------------------------------------------------
// Get the file abstraction object corresponding to the fd
//------------------------------------------------------------------------------
FileAbstraction*
xrd_get_file (int fd)
{
  eos_static_debug("fd=%i", fd);
  eos::common::RWMutexReadLock rd_lock(rwmutex_fd2fabst);
  auto iter = fd2fabst.find(fd);

  if (iter == fd2fabst.end())
  {
    eos_static_err("no file abst for fd=%i", fd);
    return 0;
  }

  iter->second->IncNumRef();
  return iter->second;
}


//------------------------------------------------------------------------------
// Remove entry from mapping
//------------------------------------------------------------------------------
int
xrd_remove_fd2file (int fd, unsigned long inode, uid_t uid)
{
  int retc = -1;
  eos_static_debug("fd=%i, inode=%lu", fd, inode);
  eos::common::RWMutexWriteLock wr_lock(rwmutex_fd2fabst);
  auto iter = fd2fabst.find(fd);

  if (iter != fd2fabst.end())
  {
    FileAbstraction* fabst = iter->second;

    if (!fabst->IsInUse())
    {
      eos_static_debug("fd=%i is not in use, remove it", fd);
      eos::fst::Layout* raw_file = fabst->GetRawFile();
      retc = raw_file->Close();
      delete fabst;
      fabst = 0;
      fd2fabst.erase(iter);

      // Remove entry also from the inodeuser2fd
      std::ostringstream sstr;
      sstr << inode << ":" << (unsigned long)uid;
      auto iter1 = inodeuser2fd.find(sstr.str());

      if (iter1 != inodeuser2fd.end())
        inodeuser2fd.erase(iter1);

      // Return fd to the pool
      pool_fd.push(fd);
    }
    else
    {
      eos_static_debug("fd=%i is still in use, cannot remove", fd);
      // Decrement number of references - so that the last process can
      // properly close the file
      fabst->DecNumRef();
      fabst->DecNumOpen();
    }
  }
  else
    eos_static_warning("fd=%i no long in map, maybe already closed ...", fd);

  return retc;
}


//------------------------------------------------------------------------------
//        ******* Implementation IO Buffer Management *******
//------------------------------------------------------------------------------

// Forward declaration
class IoBuf;

// Protecting the IO buffer map
XrdSysMutex IoBufferLock;

// IO buffer table. Each fuse thread has its own read buffer
std::map<pthread_t, IoBuf> IoBufferMap;


//------------------------------------------------------------------------------
//! Class IoBuf
//------------------------------------------------------------------------------
class IoBuf
{
private:
  void* buffer;
  size_t size;

public:

  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  IoBuf ()
  {
    buffer = 0;
    size = 0;
  }


  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual
  ~IoBuf ()
  {
    if (buffer && size) free(buffer);
  }


  //----------------------------------------------------------------------------
  //! Get buffer
  //----------------------------------------------------------------------------
  char*
  GetBuffer ()
  {
    return (char*) buffer;
  }


  //----------------------------------------------------------------------------
  //! Get size of buffer
  //----------------------------------------------------------------------------
  size_t
  GetSize ()
  {
    return size;
  }

  //----------------------------------------------------------------------------
  //! Resize buffer
  //----------------------------------------------------------------------------
  void
  Resize (size_t newsize)
  {
    if (newsize > size)
    {
      size = (newsize < (128 * 1024)) ? 128 * 1024 : newsize;
      buffer = realloc(buffer, size);
    }
  }
};


//------------------------------------------------------------------------------
// Guarantee a buffer for reading of at least 'size' for the specified thread
//------------------------------------------------------------------------------
char*
xrd_attach_rd_buff (pthread_t tid, size_t size)
{
  XrdSysMutexHelper lock(IoBufferLock);
  IoBufferMap[tid].Resize(size);
  return (char*) IoBufferMap[tid].GetBuffer();
}


//------------------------------------------------------------------------------
// Release read buffer corresponding to the thread
//------------------------------------------------------------------------------
void
xrd_release_rd_buff (pthread_t tid)
{
  XrdSysMutexHelper lock(IoBufferLock);
  IoBufferMap.erase(tid);
  return;
}


//------------------------------------------------------------------------------
//             ******* XROOTD interface functions *******
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Remove extended attribute
//------------------------------------------------------------------------------
int
xrd_rmxattr (const char* path,
             const char* xattr_name,
             uid_t uid,
             gid_t gid,
             pid_t pid)
{
  eos_static_info("path=%s xattr_name=%s uid=%u pid=%u", path, xattr_name, uid, pid);
  eos::common::Timing rmxattrtiming("rmxattr");
  COMMONTIMING("START", &rmxattrtiming);
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=rm&";
  request += "mgm.xattrname=";
  request += xattr_name;
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);

  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("GETPLUGIN", &rmxattrtiming);
  errno = 0;

  if (status.IsOK())
  {
    int retc = 0;
    int items = 0;
    char tag[1024];
    // Parse output
    items = sscanf(response->GetBuffer(), "%s retc=%i", tag, &retc);

    if ((items != 2) || (strcmp(tag, "rmxattr:")))
      errno = ENOENT;
    else
      errno = retc;
  }
  else
    errno = EFAULT;

  COMMONTIMING("END", &rmxattrtiming);

  if (EOS_LOGS_DEBUG)
    rmxattrtiming.Print();

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Set extended attribute
//------------------------------------------------------------------------------
int
xrd_setxattr (const char* path,
              const char* xattr_name,
              const char* xattr_value,
              size_t size,
              uid_t uid,
              gid_t gid,
              pid_t pid)
{
  eos_static_info("path=%s xattr_name=%s xattr_value=%s uid=%u pid=%u",
                  path, xattr_name, xattr_value, uid, pid);
  eos::common::Timing setxattrtiming("setxattr");
  COMMONTIMING("START", &setxattrtiming);
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=set&";
  request += "mgm.xattrname=";
  request += xattr_name;
  request += "&";
  request += "mgm.xattrvalue=";
  request += std::string(xattr_value, size);
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("GETPLUGIN", &setxattrtiming);
  errno = 0;

  if (status.IsOK())
  {
    int retc = 0;
    int items = 0;
    char tag[1024];
    // Parse output
    items = sscanf(response->GetBuffer(), "%s retc=%i", tag, &retc);

    if ((items != 2) || (strcmp(tag, "setxattr:")))
      errno = ENOENT;
    else
      errno = retc;
  }
  else
    errno = EFAULT;

  COMMONTIMING("END", &setxattrtiming);

  if (EOS_LOGS_DEBUG)
    setxattrtiming.Print();

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Read an extended attribute
//------------------------------------------------------------------------------
int
xrd_getxattr (const char* path,
              const char* xattr_name,
              char** xattr_value,
              size_t* size,
              uid_t uid,
              gid_t gid,
              pid_t pid)
{
  eos_static_info("path=%s xattr_name=%s uid=%u pid=%u", path, xattr_name, uid, pid);
  eos::common::Timing getxattrtiming("getxattr");

  COMMONTIMING("START", &getxattrtiming);
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=get&";
  request += "mgm.xattrname=";
  request += xattr_name;
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("GETPLUGIN", &getxattrtiming);
  errno = 0;

  if (status.IsOK())
  {
    int retc = 0;
    int items = 0;
    char tag[1024];
    char rval[4096];
    // Parse output
    items = sscanf(response->GetBuffer(), "%s retc=%i value=%s", tag, &retc, rval);

    if ((items != 3) || (strcmp(tag, "getxattr:")))
      errno = EFAULT;
    else
    {
      if (strcmp(xattr_name, "user.eos.XS") == 0)
      {
        char* ptr = rval;

        for (unsigned int i = 0; i < strlen(rval); i++, ptr++)
        {
          if (*ptr == '_')
            *ptr = ' ';
        }
      }

      *size = strlen(rval);
      *xattr_value = (char*) calloc((*size) + 1, sizeof ( char));
      *xattr_value = strncpy(*xattr_value, rval, *size);
      errno = retc;
    }
  }
  else
    errno = EFAULT;

  COMMONTIMING("END", &getxattrtiming);

  if (EOS_LOGS_DEBUG)
    getxattrtiming.Print();

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// List extended attributes
//------------------------------------------------------------------------------
int
xrd_listxattr (const char* path,
               char** xattr_list,
               size_t* size,
               uid_t uid,
               gid_t gid,
               pid_t pid)
{
  eos_static_info("path=%s uid=%u pid=%u", path, uid, pid);
  eos::common::Timing listxattrtiming("listxattr");
  COMMONTIMING("START", &listxattrtiming);
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=xattr&";
  request += "mgm.subcmd=ls";
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("GETPLUGIN", &listxattrtiming);
  errno = 0;

  if (status.IsOK())
  {
    int retc = 0;
    int items = 0;
    char tag[1024];
    char rval[16384];
    // Parse output
    items = sscanf(response->GetBuffer(), "%s retc=%i %s", tag, &retc, rval);

    if ((items != 3) || (strcmp(tag, "lsxattr:")))
      errno = ENOENT;
    else
    {
      *size = strlen(rval);
      char* ptr = rval;

      for (unsigned int i = 0; i < (*size); i++, ptr++)
      {
        if (*ptr == '&')
          *ptr = '\0';
      }

      *xattr_list = (char*) calloc((*size) + 1, sizeof ( char));
      *xattr_list = (char*) memcpy(*xattr_list, rval, *size);
      errno = retc;
    }
  }
  else
    errno = EFAULT;

  COMMONTIMING("END", &listxattrtiming);

  if (EOS_LOGS_DEBUG)
    listxattrtiming.Print();

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Return file attributes. If a field is meaningless or semi-meaningless
// (e.g., st_ino) then it should be set to 0 or given a "reasonable" value.
//------------------------------------------------------------------------------
int
xrd_stat (const char* path,
          struct stat* buf,
          uid_t uid,
          gid_t gid,
          unsigned long inode)
{
  eos_static_info("path=%s, uid=%i, gid=%i inode=%lu",
                  path, (int)uid, (int)gid, inode);
  eos::common::Timing stattiming("xrd_stat");
  off_t file_size = -1;
  errno = 0;
  COMMONTIMING("START", &stattiming);

  if (inode)
  {
    // Try to stat via an open file - first find the file descriptor using the
    // inodeuser2fd map and then find the file object using the fd2fabst map.
    // Meanwhile keep the mutex locked for read so that no other thread can
    // delete the file object
    eos_static_debug("path=%s, uid=%lu, inode=%lu",
                     path, (unsigned long) uid, inode);
    eos::common::RWMutexReadLock rd_lock(rwmutex_fd2fabst);
    std::ostringstream sstr;
    sstr << inode << ":" << (unsigned long)uid;
    google::dense_hash_map<std::string, int>::iterator
      iter_fd = inodeuser2fd.find(sstr.str());

    if (iter_fd != inodeuser2fd.end())
    {
      google::dense_hash_map<int, FileAbstraction*>::iterator
        iter_file = fd2fabst.find(iter_fd->second);

      if (iter_file != fd2fabst.end())
      {
        // Force flush so that we get the real current size through the file obj.
        if (XFC && fuse_cache_write)
          XFC->ForceAllWrites(iter_file->second);

        struct stat tmp;
        eos::fst::Layout* file = iter_file->second->GetRawFile();

        if (!file->Stat(&tmp))
        {
          file_size = tmp.st_size;
          eos_static_debug("fd=%i, size-fd=%lld, raw_file=%p",
                          iter_fd->second, file_size, file);
        }
        else
          eos_static_err("fd=%i stat failed on open file", iter_fd->second);
      }
      else
        eos_static_err("fd=%i not found in file obj map", iter_fd->second);
    }
    else
      eos_static_debug("path=%s not open", path);
  }

  // Do stat using the Fils System object
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=stat";
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, 0));
  XrdCl::FileSystem fs(Url);

  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("GETPLUGIN", &stattiming);

  if (status.IsOK())
  {
    unsigned long long sval[10];
    unsigned long long ival[6];
    char tag[1024];
    // Parse output
    int items = sscanf(response->GetBuffer(),
                       "%s %llu %llu %llu %llu %llu %llu %llu %llu "
                       "%llu %llu %llu %llu %llu %llu %llu %llu",
                       tag, (unsigned long long*) &sval[0],
                       (unsigned long long*) &sval[1],
                       (unsigned long long*) &sval[2],
                       (unsigned long long*) &sval[3],
                       (unsigned long long*) &sval[4],
                       (unsigned long long*) &sval[5],
                       (unsigned long long*) &sval[6],
                       (unsigned long long*) &sval[7],
                       (unsigned long long*) &sval[8],
                       (unsigned long long*) &sval[9],
                       (unsigned long long*) &ival[0],
                       (unsigned long long*) &ival[1],
                       (unsigned long long*) &ival[2],
                       (unsigned long long*) &ival[3],
                       (unsigned long long*) &ival[4],
                       (unsigned long long*) &ival[5]);

    if ((items != 17) || (strcmp(tag, "stat:")))
    {
      errno = ENOENT;
      delete response;
      return errno;
    }
    else
    {
      buf->st_dev = (dev_t) sval[0];
      buf->st_ino = (ino_t) sval[1];
      buf->st_mode = (mode_t) sval[2];
      buf->st_nlink = (nlink_t) sval[3];
      buf->st_uid = (uid_t) sval[4];
      buf->st_gid = (gid_t) sval[5];
      buf->st_rdev = (dev_t) sval[6];
      buf->st_size = (off_t) sval[7];
      buf->st_blksize = (blksize_t) sval[8];
      buf->st_blocks = (blkcnt_t) sval[9];
#ifdef __APPLE__
      buf->st_atimespec.tv_sec = (time_t) ival[0];
      buf->st_mtimespec.tv_sec = (time_t) ival[1];
      buf->st_ctimespec.tv_sec = (time_t) ival[2];
      buf->st_atimespec.tv_nsec = (time_t) ival[3];
      buf->st_mtimespec.tv_nsec = (time_t) ival[4];
      buf->st_ctimespec.tv_nsec = (time_t) ival[5];
#else
      buf->st_atime = (time_t) ival[0];
      buf->st_mtime = (time_t) ival[1];
      buf->st_ctime = (time_t) ival[2];
      buf->st_atim.tv_sec = (time_t) ival[0];
      buf->st_mtim.tv_sec = (time_t) ival[1];
      buf->st_ctim.tv_sec = (time_t) ival[2];
      buf->st_atim.tv_nsec = (time_t) ival[3];
      buf->st_mtim.tv_nsec = (time_t) ival[4];
      buf->st_ctim.tv_nsec = (time_t) ival[5];
#endif

      if (S_ISREG(buf->st_mode) && fuse_exec)
        buf->st_mode |= (S_IXUSR | S_IXGRP | S_IXOTH);

      buf->st_mode &= (~S_ISVTX); // clear the vxt bit
      buf->st_mode &= (~S_ISUID); // clear suid
      buf->st_mode &= (~S_ISGID); // clear sgid
      errno = 0;
    }
  }
  else
  {
    eos_static_err("error=status is NOT ok");
    errno = EFAULT;
  }

  // If got size using opened file then return this value
  if (file_size != -1)
    buf->st_size = file_size;

  COMMONTIMING("END", &stattiming);

  if (EOS_LOGS_DEBUG)
    stattiming.Print();

  eos_static_info("path=%s st-size=%llu errno=%i", path, buf->st_size, errno);
  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Return statistics about the filesystem
//------------------------------------------------------------------------------
int
xrd_statfs (const char* path, struct statvfs* stbuf)
{
  eos_static_info("path=%s", path);
  static unsigned long long a1 = 0;
  static unsigned long long a2 = 0;
  static unsigned long long a3 = 0;
  static unsigned long long a4 = 0;
  static XrdSysMutex statmutex;
  static time_t laststat = 0;
  statmutex.Lock();
  errno = 0;

  if ((time(NULL) - laststat) < ((15 + (int) 5.0 * rand() / RAND_MAX)))
  {
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = a3 / 4096;
    stbuf->f_bfree = a1 / 4096;
    stbuf->f_bavail = a1 / 4096;
    stbuf->f_files = a4;
    stbuf->f_ffree = a2;
    stbuf->f_fsid = 0xcafe;
    stbuf->f_namemax = 256;
    statmutex.UnLock();
    return errno;
  }

  eos::common::Timing statfstiming("xrd_statfs");
  COMMONTIMING("START", &statfstiming);

  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=statvfs&";
  request += "path=";
  request += path;
  arg.FromString(request);
  XrdCl::URL Url(xrd_user_url(2, 2, 0));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  errno = 0;

  if (status.IsOK())
  {
    int retc;
    char tag[1024];

    if (!response->GetBuffer())
    {
      statmutex.UnLock();
      delete response;
      errno = EFAULT;
      return errno;
    }

    // Parse output
    int items = sscanf(response->GetBuffer(),
                       "%s retc=%d f_avail_bytes=%llu f_avail_files=%llu "
                       "f_max_bytes=%llu f_max_files=%llu",
                       tag, &retc, &a1, &a2, &a3, &a4);

    if ((items != 6) || (strcmp(tag, "statvfs:")))
    {
      statmutex.UnLock();
      delete response;
      errno = EFAULT;
      return errno;
    }

    errno = retc;
    laststat = time(NULL);
    statmutex.UnLock();
    stbuf->f_bsize = 4096;
    stbuf->f_frsize = 4096;
    stbuf->f_blocks = a3 / 4096;
    stbuf->f_bfree = a1 / 4096;
    stbuf->f_bavail = a1 / 4096;
    stbuf->f_files = a4;
    stbuf->f_ffree = a2;
  }
  else
  {
    statmutex.UnLock();
    errno = EFAULT;
  }

  COMMONTIMING("END", &statfstiming);

  if (EOS_LOGS_DEBUG)
    statfstiming.Print();

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Change permissions for the file
//------------------------------------------------------------------------------
int
xrd_chmod (const char* path,
           mode_t mode,
           uid_t uid,
           gid_t gid,
           pid_t pid)
{
  eos_static_info("path=%s mode=%x uid=%u pid=%u", path, mode, uid, pid);
  eos::common::Timing chmodtiming("xrd_chmod");
  COMMONTIMING("START", &chmodtiming);
  int retc = 0;
  XrdOucString smode;
  smode += (int) mode;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=chmod&mode=";
  request += smode.c_str();
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("END", &chmodtiming);
  errno = 0;

  if (EOS_LOGS_DEBUG)
    chmodtiming.Print();

  if (status.IsOK())
  {
    char tag[1024];

    if (!response->GetBuffer())
    {
      delete response;
      errno = EFAULT;
      return errno;
    }

    // Parse output
    int items = sscanf(response->GetBuffer(), "%s retc=%d", tag, &retc);

    if ((items != 2) || (strcmp(tag, "chmod:")))
      errno = EFAULT;
    else
      errno = retc;
  }
  else
    errno = EFAULT;

  delete response;
  return errno;
}

//------------------------------------------------------------------------------
// Update the last access time and last modification time
//------------------------------------------------------------------------------
int
xrd_utimes (const char* path,
            struct timespec* tvp,
            uid_t uid,
            gid_t gid,
            pid_t pid)
{
  eos_static_info("path=%s uid=%u pid=%u", path, uid, pid);
  eos::common::Timing utimestiming("xrd_utimes");

  COMMONTIMING("START", &utimestiming);
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=utimes&tv1_sec=";
  char lltime[1024];
  sprintf(lltime, "%llu", (unsigned long long) tvp[0].tv_sec);
  request += lltime;
  request += "&tv1_nsec=";
  sprintf(lltime, "%llu", (unsigned long long) tvp[0].tv_nsec);
  request += lltime;
  request += "&tv2_sec=";
  sprintf(lltime, "%llu", (unsigned long long) tvp[1].tv_sec);
  request += lltime;
  request += "&tv2_nsec=";
  sprintf(lltime, "%llu", (unsigned long long) tvp[1].tv_nsec);
  request += lltime;
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("END", &utimestiming);
  errno = 0;

  if (EOS_LOGS_DEBUG)
    utimestiming.Print();

  if (status.IsOK())
  {
    int retc = 0;
    char tag[1024];
    // Parse output
    int items = sscanf(response->GetBuffer(), "%s retc=%d", tag, &retc);

    if ((items != 2) || (strcmp(tag, "utimes:")))
      errno = EFAULT;
    else
      errno = retc;
  }
  else
    errno = EFAULT;

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// It returns -ENOENT if the path doesn't exist, -EACCESS if the requested
// permission isn't available, or 0 for success. Note that it can be called
// on files, directories, or any other object that appears in the filesystem.
//------------------------------------------------------------------------------
int
xrd_access (const char* path,
            int mode,
            uid_t uid,
            gid_t gid,
            pid_t pid
            )
{
  eos_static_info("path=%s mode=%d uid=%u pid=%u", path, mode, uid, pid);
  eos::common::Timing accesstiming("xrd_access");
  COMMONTIMING("START", &accesstiming);

  int retc = 0;
  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  char smode[16];
  snprintf(smode, sizeof (smode) - 1, "%d", mode);
  request = path;
  request += "?";
  request += "mgm.pcmd=access&mode=";
  request += smode;
  arg.FromString(request);
  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);

  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);

  COMMONTIMING("STOP", &accesstiming);
  errno = 0;

  if (EOS_LOGS_DEBUG)
    accesstiming.Print();

  if (status.IsOK())
  {
    char tag[1024];
    // Parse output
    int items = sscanf(response->GetBuffer(), "%s retc=%d", tag, &retc);

    if (EOS_LOGS_DEBUG)
      fprintf(stderr, "access-retc=%d\n", retc);

    if ((items != 2) || (strcmp(tag, "access:")))
      errno = EFAULT;
    else
      errno = retc;
  }
  else
    errno = EFAULT;

  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Get list of entries in directory
//------------------------------------------------------------------------------
int
xrd_inodirlist (unsigned long long dirinode,
                const char* path,
                uid_t uid,
                gid_t gid,
                pid_t pid)
{
  eos_static_info("inode=%llu path=%s", dirinode, path);
  eos::common::Timing inodirtiming("xrd_inodirlist");
  COMMONTIMING("START", &inodirtiming);

  int retc = 0;
  char* ptr = 0;
  char* value = 0;
  int doinodirlist = -1;
  std::string request = path;

  // we have to replace '&' in path names with '#AND#'

  size_t a_pos = request.find("mgm.path=/");

  while ( (a_pos = request.find("&", a_pos+1)) != std::string::npos)
  {
    request.erase(a_pos,1);
    request.insert(a_pos,"#AND#");
    a_pos+=4;
  }

  COMMONTIMING("GETSTSTREAM", &inodirtiming);
  request.insert(0, xrd_user_url(uid, gid, pid));
  XrdCl::File* file = new XrdCl::File();
  XrdCl::XRootDStatus status = file->Open(request.c_str(),
                                          XrdCl::OpenFlags::Flags::Read);
  errno = 0;

  if (!status.IsOK())
  {
    eos_static_err("error=got an error to request.");
    delete file;
    errno = ENOENT;
    return errno;
  }

  // Start to read
  int npages = 1;
  off_t offset = 0;
  unsigned int nbytes = 0;
  value = (char*) malloc(PAGESIZE + 1);
  COMMONTIMING("READSTSTREAM", &inodirtiming);

  status = file->Read(offset, PAGESIZE, value + offset, nbytes);

  while ((status.IsOK()) && (nbytes == PAGESIZE))
  {
    npages++;
    value = (char*) realloc(value, npages * PAGESIZE + 1);
    offset += PAGESIZE;
    status = file->Read(offset, PAGESIZE, value + offset, nbytes);
  }

  if (status.IsOK()) offset += nbytes;

  value[offset] = 0;
  delete file;
  xrd_dirview_create((unsigned long long) dirinode);
  COMMONTIMING("PARSESTSTREAM", &inodirtiming);
  xrd_lock_w_dirview(); // =>

  if (status.IsOK())
  {
    char dirpath[4096];
    unsigned long long inode;
    char tag[128];
    // Parse output
    int items = sscanf(value, "%s retc=%d", tag, &retc);

    if (retc)
    {
      free(value);
      xrd_unlock_w_dirview(); // <=
      xrd_dirview_delete((unsigned long long) dirinode);
      errno = EFAULT;
      return errno;
    }

    if ((items != 2) || (strcmp(tag, "inodirlist:")))
    {
      eos_static_err("error=got an error(1).");
      free(value);
      xrd_unlock_w_dirview(); // <=
      xrd_dirview_delete((unsigned long long) dirinode);
      errno = EFAULT;
      return errno;
    }

    ptr = strchr(value, ' ');
    if (ptr) ptr = strchr(ptr + 1, ' ');
    char* endptr = value + strlen(value) - 1;

    while ((ptr) && (ptr < endptr))
    {
      int items = sscanf(ptr, "%s %llu", dirpath, &inode);

      if (items != 2)
      {
        eos_static_err("error=got an error(2).");
        free(value);
        xrd_unlock_w_dirview(); // <=
        xrd_dirview_delete((unsigned long long) dirinode);
        errno = EFAULT;
        return errno;
      }

      XrdOucString whitespacedirpath = dirpath;
      whitespacedirpath.replace("%20", " ");

      xrd_store_child_p2i(dirinode, inode, whitespacedirpath.c_str());
      dir2inodelist[dirinode].push_back(inode);

      // to the next entries
      if (ptr) ptr = strchr(ptr + 1, ' ');
      if (ptr) ptr = strchr(ptr + 1, ' ');
    }

    doinodirlist = 0;
  }

  xrd_unlock_w_dirview(); // <=
  COMMONTIMING("END", &inodirtiming);

  if (EOS_LOGS_DEBUG)
    //inodirtiming.Print();

  free(value);
  return doinodirlist;
}


//------------------------------------------------------------------------------
// Get directory entries
//------------------------------------------------------------------------------
struct dirent*
xrd_readdir (const char* path_dir, size_t *size,
             uid_t uid,
             gid_t gid,
             pid_t pid)
{
  eos_static_info("path=%s", path_dir);
  struct dirent* dirs = NULL;
  XrdCl::DirectoryList* response = 0;
  XrdCl::DirListFlags::Flags flags = XrdCl::DirListFlags::None;
  string path_str = path_dir;

  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.DirList(path_str, flags, response);

  if (status.IsOK())
  {
    *size = response->GetSize();
    dirs = static_cast<struct dirent*> (calloc(*size, sizeof ( struct dirent)));
    int i = 0;

    for (XrdCl::DirectoryList::ConstIterator iter = response->Begin();
      iter != response->End();
      ++iter)
    {
      XrdCl::DirectoryList::ListEntry* list_entry =
        static_cast<XrdCl::DirectoryList::ListEntry*> (*iter);
      size_t len = list_entry->GetName().length();
      const char* cp = list_entry->GetName().c_str();
      const int dirhdrln = dirs[i].d_name - (char *) &dirs[i];
#if defined(__macos__) || defined(__FreeBSD__)
      dirs[i].d_fileno = i;
      dirs[i].d_type = DT_UNKNOWN;
      dirs[i].d_namlen = len;
#else
      dirs[i].d_ino = i;
      dirs[i].d_off = i*NAME_MAX;
#endif
      dirs[i].d_reclen = len + dirhdrln;
      dirs[i].d_type = DT_UNKNOWN;
      strncpy(dirs[i].d_name, cp, len);
      dirs[i].d_name[len] = '\0';
      i++;
    }

    return dirs;
  }

  *size = 0;
  return NULL;
}


//------------------------------------------------------------------------------
// Create a directory with the given name
//------------------------------------------------------------------------------
int
xrd_mkdir (const char* path,
           mode_t mode,
           uid_t uid,
           gid_t gid,
           pid_t pid,
           struct stat* buf)
{
  eos_static_info("path=%s mode=%d uid=%u pid=%u", path, mode, uid, pid);
  eos::common::Timing mkdirtiming("xrd_mkdir");
  errno = 0;
  COMMONTIMING("START", &mkdirtiming);

  std::string request;
  XrdCl::Buffer arg;
  XrdCl::Buffer* response = 0;
  request = path;
  request += "?";
  request += "mgm.pcmd=mkdir";
  request += "&mode=";
  request += (int) mode;
  arg.FromString(request);

  XrdCl::URL Url(xrd_user_url(uid, gid, 0));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Query(XrdCl::QueryCode::OpaqueFile,
                                        arg, response);
  COMMONTIMING("GETPLUGIN", &mkdirtiming);

  if (status.IsOK())
  {
    unsigned long long sval[10];
    unsigned long long ival[6];
    char tag[1024];
    //..........................................................................
    // Parse output
    //..........................................................................
    int items = sscanf(response->GetBuffer(),
                       "%s %llu %llu %llu %llu %llu %llu %llu %llu "
                       "%llu %llu %llu %llu %llu %llu %llu %llu",
                       tag, (unsigned long long*) &sval[0],
                       (unsigned long long*) &sval[1],
                       (unsigned long long*) &sval[2],
                       (unsigned long long*) &sval[3],
                       (unsigned long long*) &sval[4],
                       (unsigned long long*) &sval[5],
                       (unsigned long long*) &sval[6],
                       (unsigned long long*) &sval[7],
                       (unsigned long long*) &sval[8],
                       (unsigned long long*) &sval[9],
                       (unsigned long long*) &ival[0],
                       (unsigned long long*) &ival[1],
                       (unsigned long long*) &ival[2],
                       (unsigned long long*) &ival[3],
                       (unsigned long long*) &ival[4],
                       (unsigned long long*) &ival[5]);

    if ((items != 17) || (strcmp(tag, "mkdir:")))
    {
      errno = ENOENT;
      delete response;
      return errno;
    }
    else
    {
      buf->st_dev = (dev_t) sval[0];
      buf->st_ino = (ino_t) sval[1];
      buf->st_mode = (mode_t) sval[2];
      buf->st_nlink = (nlink_t) sval[3];
      buf->st_uid = (uid_t) sval[4];
      buf->st_gid = (gid_t) sval[5];
      buf->st_rdev = (dev_t) sval[6];
      buf->st_size = (off_t) sval[7];
      buf->st_blksize = (blksize_t) sval[8];
      buf->st_blocks = (blkcnt_t) sval[9];
#ifdef __APPLE__
      buf->st_atimespec.tv_sec = (time_t) ival[0];
      buf->st_mtimespec.tv_sec = (time_t) ival[1];
      buf->st_ctimespec.tv_sec = (time_t) ival[2];
      buf->st_atimespec.tv_nsec = (time_t) ival[3];
      buf->st_mtimespec.tv_nsec = (time_t) ival[4];
      buf->st_ctimespec.tv_nsec = (time_t) ival[5];
#else
      buf->st_atime = (time_t) ival[0];
      buf->st_mtime = (time_t) ival[1];
      buf->st_ctime = (time_t) ival[2];
      buf->st_atim.tv_sec = (time_t) ival[0];
      buf->st_mtim.tv_sec = (time_t) ival[1];
      buf->st_ctim.tv_sec = (time_t) ival[2];
      buf->st_atim.tv_nsec = (time_t) ival[3];
      buf->st_mtim.tv_nsec = (time_t) ival[4];
      buf->st_ctim.tv_nsec = (time_t) ival[5];
#endif

      if (S_ISREG(buf->st_mode) && fuse_exec)
        buf->st_mode |= (S_IXUSR | S_IXGRP | S_IXOTH);

      buf->st_mode &= (~S_ISVTX); // clear the vxt bit
      buf->st_mode &= (~S_ISUID); // clear suid
      buf->st_mode &= (~S_ISGID); // clear sgid
      errno = 0;
    }
  }
  else
  {
    eos_static_err("error=status is NOT ok");
    errno = EFAULT;
  }

  COMMONTIMING("END", &mkdirtiming);

  if (EOS_LOGS_DEBUG)
    mkdirtiming.Print();

  eos_static_debug("path=%s inode=%llu", path, buf->st_ino);
  delete response;
  return errno;
}


//------------------------------------------------------------------------------
// Remove the given directory
//------------------------------------------------------------------------------
int
xrd_rmdir (const char* path, uid_t uid, gid_t gid, pid_t pid)
{
  eos_static_info("path=%s uid=%u pid=%u", path, uid, pid);
  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.RmDir(path);

  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}


//------------------------------------------------------------------------------
// Map open return codes to errno's
//------------------------------------------------------------------------------
int
xrd_error_retc_map (int retc)
{
  if (retc) errno = retc;
  if (retc == kXR_ArgInvalid)
    errno = EINVAL;

  if (retc == kXR_ArgMissing)
    errno = EINVAL;

  if (retc == kXR_ArgTooLong)
    errno = E2BIG;

  if (retc == kXR_FileNotOpen)
    errno = EBADF;

  if (retc == kXR_FSError)
    errno = EIO;

  if (retc == kXR_InvalidRequest)
    errno = EINVAL;

  if (retc == kXR_IOError)
    errno = EIO;

  if (retc == kXR_NoMemory)
    errno = ENOMEM;

  if (retc == kXR_NoSpace)
    errno = ENOSPC;

  if (retc == kXR_ServerError)
    errno = EIO;

  if (retc == kXR_NotAuthorized)
    errno = EPERM;

  if (retc == kXR_NotFound)
    errno = ENOENT;

  if (retc == kXR_Unsupported)
    errno = ENOTSUP;

  if (retc == kXR_NotFile)
    errno = EISDIR;

  if (retc == kXR_isDirectory)
    errno = EISDIR;

  if (retc == kXR_Cancelled)
    errno = ECANCELED;

  if (retc == kXR_ChkLenErr)
    errno = ERANGE;

  if (retc == kXR_ChkSumErr)
    errno = ERANGE;

  if (retc == kXR_inProgress)
    errno = EAGAIN;

  if (retc)
    return -1;

  return 0;
}

//------------------------------------------------------------------------------
// Open a file
//------------------------------------------------------------------------------
int
xrd_open (const char* path,
          int oflags,
          mode_t mode,
          uid_t uid,
          gid_t gid,
          pid_t pid,
          unsigned long* return_inode)
{
  eos_static_info("path=%s flags=%08x mode=%d uid=%u pid=%u",
                  path, oflags, mode, uid, pid);
  XrdOucString spath = xrd_user_url(uid, gid, pid);
  XrdSfsFileOpenMode flags_sfs = eos::common::LayoutId::MapFlagsPosix2Sfs(oflags);
  spath += path;
  errno = 0;
  int t0;
  int retc = xrd_add_fd2file(0, *return_inode, uid);

  if (retc != -1)
  {
    eos_static_debug("file already opened, return fd=%i", retc);
    return retc;
  }


  if ((t0 = spath.find("/proc/")) != STR_NPOS)
  {
    XrdOucString orig_path=spath;
    // Clean the path
    int t1 = spath.find("//");
    int t2 = spath.find("//", t1 + 2);
    spath.erase(t2 + 2, t0 - t2 - 2);

    while (spath.replace("///", "//")) { };

    // Force a reauthentication to the head node
    if (spath.endswith("/proc/reconnect"))
    {
      XrdSysMutexHelper cLock(connectionIdMutex);
      connectionId++;
      errno = ECONNABORTED;
      return -1;
    }

    // Return the 'whoami' information in that file
    if (spath.endswith("/proc/whoami"))
    {
      spath.replace("/proc/whoami", "/proc/user/");
      spath += "?mgm.cmd=whoami&mgm.format=fuse&eos.app=fuse";
      eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                         eos::common::LayoutId::kXrdCl);

      XrdOucString open_path = get_url_nocgi(spath.c_str());
      XrdOucString open_cgi = get_cgi(spath.c_str());

      retc = file->Open(open_path.c_str(), flags_sfs, mode, open_cgi.c_str());

      if (retc)
      {
        eos_static_err("error=open failed for %s", spath.c_str());
        return xrd_error_retc_map(errno);
      }
      else
      {
        retc = xrd_add_fd2file(file, *return_inode, uid);
        return retc;
      }
    }

    if (spath.endswith("/proc/who"))
    {
      spath.replace("/proc/who", "/proc/user/");
      spath += "?mgm.cmd=who&mgm.format=fuse&eos.app=fuse";
      eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                         eos::common::LayoutId::kXrdCl);
      XrdOucString open_path = get_url_nocgi(spath.c_str());
      XrdOucString open_cgi = get_cgi(spath.c_str());

      retc = file->Open(open_path.c_str(), flags_sfs, mode, open_cgi.c_str());

      if (retc)
      {
        eos_static_err("error=open failed for %s", spath.c_str());
        return xrd_error_retc_map(errno);
      }
      else
      {
        retc = xrd_add_fd2file(file, *return_inode, uid);
        return retc;
      }
    }

    if (spath.endswith("/proc/quota"))
    {
      spath.replace("/proc/quota", "/proc/user/");
      spath += "?mgm.cmd=quota&mgm.subcmd=lsuser&mgm.format=fuse&eos.app=fuse";
      eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                         eos::common::LayoutId::kXrdCl);

      XrdOucString open_path = get_url_nocgi(spath.c_str());
      XrdOucString open_cgi = get_cgi(spath.c_str());
      retc = file->Open(open_path.c_str(), flags_sfs, mode, open_cgi.c_str());

      if (retc)
      {
        eos_static_err("error=open failed for %s", spath.c_str());
        return xrd_error_retc_map(errno);
      }
      else
      {
        retc = xrd_add_fd2file(file, *return_inode, uid);
        return retc;
      }
    }
    spath=orig_path;
  }

  // Try to open file using PIO (parallel io) only in read mode
  if ((!getenv("EOS_FUSE_NOPIO")) && (flags_sfs == SFS_O_RDONLY))
  {
    XrdCl::Buffer arg;
    XrdCl::Buffer* response = 0;
    XrdCl::XRootDStatus status;
    std::string file_path = path;
    size_t spos = file_path.rfind("//");

    if (spos != std::string::npos)
      file_path.erase(0, spos + 1);

    std::string request = file_path;
    request += "?mgm.pcmd=open";
    arg.FromString(request);

    XrdCl::URL Url(xrd_user_url(uid, gid, pid));
    XrdCl::FileSystem fs(Url);
    status = fs.Query(XrdCl::QueryCode::OpaqueFile, arg, response);

    if (status.IsOK())
    {
      // Parse output
      XrdOucString tag;
      XrdOucString stripePath;
      std::vector<std::string> stripeUrls;

      XrdOucString origResponse = response->GetBuffer();
      XrdOucString stringOpaque = response->GetBuffer();

      // Add the eos.app=fuse tag to all future PIO open requests
      origResponse += "&eos.app=fuse";

      while (stringOpaque.replace("?", "&")) {}

      while (stringOpaque.replace("&&", "&")) {}

      XrdOucEnv* openOpaque = new XrdOucEnv(stringOpaque.c_str());
      char* opaqueInfo = (char*) strstr(origResponse.c_str(), "&mgm.logid");

      if (opaqueInfo)
      {
        opaqueInfo += 1;
        LayoutId::layoutid_t layout = openOpaque->GetInt("mgm.lid");

        for (unsigned int i = 0; i <= eos::common::LayoutId::GetStripeNumber(layout); i++)
        {
          tag = "pio.";
          tag += static_cast<int> (i);
          stripePath = "root://";
          stripePath += openOpaque->Get(tag.c_str());
          stripePath += "/";
          stripePath += file_path.c_str();
          stripeUrls.push_back(stripePath.c_str());
        }

        eos::fst::RaidMetaLayout* file;

        if (LayoutId::GetLayoutType(layout) == LayoutId::kRaidDP)
        {
          file = new eos::fst::RaidDpLayout(NULL, layout, NULL, NULL,
                                            eos::common::LayoutId::kXrdCl);
        }
        else if ((LayoutId::GetLayoutType(layout) == LayoutId::kRaid6) ||
                 (LayoutId::GetLayoutType(layout) == LayoutId::kArchive))
        {
          file = new eos::fst::ReedSLayout(NULL, layout, NULL, NULL,
                                           eos::common::LayoutId::kXrdCl);
        }
        else
        {
          eos_static_warning("warning=no such supported layout for PIO");
          file = 0;
        }

        if (file)
        {
          retc = file->OpenPio(stripeUrls,
                               flags_sfs,
                               mode,
                               opaqueInfo);
          if (retc)
          {
            eos_static_err("error=failed open for pio red, path=%s", spath.c_str());
            delete file;
            return xrd_error_retc_map(errno);
          }
          else
          {
            if (return_inode)
            {
              // Try to extract the inode from the opaque redirection
              XrdOucEnv RedEnv = file->GetLastUrl().c_str();
              const char* sino = RedEnv.Get("mgm.id");

              if (sino)
                *return_inode = eos::common::FileId::Hex2Fid(sino) << 28;
              else
                *return_inode = 0;

              eos_static_debug("path=%s created inode=%lu", path,
                               (unsigned long)*return_inode);
            }

            retc = xrd_add_fd2file(file, *return_inode, uid);
            return retc;
          }
        }
      }
      else
        eos_static_debug("error=opaque info not what we expected");
    }
    else
      eos_static_err("error=failed get request for pio read");
  }

  eos_static_debug("the spath is:%s", spath.c_str());
  eos::fst::Layout* file = new eos::fst::PlainLayout(NULL, 0, NULL, NULL,
                                                     eos::common::LayoutId::kXrdCl);
  retc = file->Open(spath.c_str(), flags_sfs, mode, "eos.app=fuse&eos.bookingsize=0");

  if (retc)
  {
    eos_static_err("error=open failed for %s.", spath.c_str());
    delete file;
    return xrd_error_retc_map(errno);
  }
  else
  {
    if (return_inode)
    {
      // Try to extract the inode from the opaque redirection
      XrdOucEnv RedEnv = file->GetLastUrl().c_str();
      const char* sino = RedEnv.Get("mgm.id");

      if (sino)
        *return_inode = eos::common::FileId::Hex2Fid(sino) << 28;
      else
        *return_inode = 0;

      eos_static_debug("path=%s created ino=%lu", path, (unsigned long)*return_inode);
    }

    retc = xrd_add_fd2file(file, *return_inode, uid);
    return retc;
  }
}


//------------------------------------------------------------------------------
// Release is called when FUSE is completely done with a file; at that point,
// you can free up any temporarily allocated data structures.
//------------------------------------------------------------------------------
int
xrd_close (int fildes, unsigned long inode, uid_t uid)
{
  eos_static_info("fd=%d inode=%lu, uid=%i", fildes, inode, uid);
  int ret = -1;
  FileAbstraction* fabst = xrd_get_file(fildes);

  if (!fabst)
  {
    errno = ENOENT;
    return ret;
  }

  if (XFC)
    XFC->ForceAllWrites(fabst);

  // Close file and remove it from all mappings
  ret = xrd_remove_fd2file(fildes, inode, uid);

  if (ret)
    errno = EIO;

  return ret;
}


//------------------------------------------------------------------------------
// Flush file data to disk
//------------------------------------------------------------------------------
int
xrd_flush (int fd)
{
  int retc = 0;
  eos_static_info("fd=%d ", fd);
  FileAbstraction* fabst = xrd_get_file(fd);

  if (!fabst)
  {
    errno = ENOENT;
    return -1;
  }

  if (XFC && fuse_cache_write)
  {
    XFC->ForceAllWrites(fabst);
    eos::common::ConcurrentQueue<error_type> err_queue = fabst->GetErrorQueue();
    error_type error;

    if (err_queue.try_pop(error))
    {
      eos_static_info("Extract error from queue");
      retc = error.first;
    }
  }

  fabst->DecNumRef();
  return retc;
}


//------------------------------------------------------------------------------
// Truncate file
//------------------------------------------------------------------------------
int
xrd_truncate (int fildes, off_t offset)
{
  int ret = -1;
  eos_static_info("fd=%d offset=%llu", fildes, (unsigned long long) offset);
  FileAbstraction* fabst = xrd_get_file(fildes);
  errno = 0;

  if (!fabst)
  {
    errno = ENOENT;
    return ret;
  }

  if (XFC && fuse_cache_write)
    XFC->ForceAllWrites(fabst);

  eos::fst::Layout* file = fabst->GetRawFile();
  ret = file->Truncate(offset);
  fabst->DecNumRef();

  if (ret == -1)
    errno = EIO;

  return ret;
}


//------------------------------------------------------------------------------
// Read from file. Returns the number of bytes transferred, or 0 if offset
// was at or beyond the end of the file
//------------------------------------------------------------------------------
ssize_t
xrd_pread (int fildes,
           void* buf,
           size_t nbyte,
           off_t offset)
{
  eos::common::Timing xpr("xrd_pread");
  COMMONTIMING("start", &xpr);

  eos_static_debug("fd=%d nbytes=%lu offset=%llu",
                   fildes, (unsigned long) nbyte,
                   (unsigned long long) offset);
  int64_t ret = -1;
  FileAbstraction* fabst = xrd_get_file(fildes);

  if (!fabst)
  {
    errno = ENOENT;
    return ret;
  }

  if (XFC && fuse_cache_write)
  {
    fabst->mMutexRW.WriteLock();
    XFC->ForceAllWrites(fabst);
    eos::fst::Layout* file = fabst->GetRawFile();
    ret = file->Read(offset, static_cast<char*> (buf), nbyte);
    fabst->mMutexRW.UnLock();
  }
  else
  {
    eos::fst::Layout* file = fabst->GetRawFile();
    ret = file->Read(offset, static_cast<char*> (buf), nbyte);
  }

  // Release file reference
  fabst->DecNumRef();
  COMMONTIMING("end", &xpr);

  if (ret == -1)
  {
    eos_static_err("error=failed to do read");
    errno = EIO;
  }

  if (EOS_LOGS_DEBUG)
    xpr.Print();

  return ret;
}


//------------------------------------------------------------------------------
// Write to file
//------------------------------------------------------------------------------
ssize_t
xrd_pwrite (int fildes,
            const void* buf,
            size_t nbyte,
            off_t offset)
{
  eos::common::Timing xpw("xrd_pwrite");
  COMMONTIMING("start", &xpw);
  eos_static_debug("fd=%d nbytes=%lu cache=%d cache-w=%d",
                   fildes, (unsigned long) nbyte, XFC ? 1 : 0,
                   fuse_cache_write);
  int64_t ret = -1;
  FileAbstraction* fabst = xrd_get_file(fildes);

  if (!fabst)
  {
    errno = ENOENT;
    return ret;
  }


  if (XFC && fuse_cache_write)
  {
    fabst->mMutexRW.ReadLock();
    XFC->SubmitWrite(fabst, const_cast<void*> (buf), offset, nbyte);
    fabst->mMutexRW.UnLock();
    ret = nbyte;
  }
  else
  {
    eos::fst::Layout* file = fabst->GetRawFile();
    ret = file->Write(offset, static_cast<const char*> (buf), nbyte);

    if (ret == -1)
      errno = EIO;
  }

  // Release file reference
  fabst->DecNumRef();
  COMMONTIMING("end", &xpw);

  if (EOS_LOGS_DEBUG)
    xpw.Print();

  return ret;
}


//------------------------------------------------------------------------------
// Flush any dirty information about the file to disk
//------------------------------------------------------------------------------
int
xrd_fsync (int fildes)
{
  eos_static_info("fd=%d", fildes);
  int ret = -1;
  FileAbstraction* fabst = xrd_get_file(fildes);

  if (!fabst)
  {
    errno = ENOENT;
    return ret;
  }

  if (XFC && fuse_cache_write)
    XFC->ForceAllWrites(fabst);

  eos::fst::Layout* file = fabst->GetRawFile();
  ret = file->Sync();

  if (ret)
    errno = EIO;

  // Release file reference
  fabst->DecNumRef();
  return ret;
}


//------------------------------------------------------------------------------
// Remove (delete) the given file, symbolic link, hard link, or special node
//------------------------------------------------------------------------------
int
xrd_unlink (const char* path,
            uid_t uid,
            gid_t gid,
            pid_t pid)
{
  eos_static_info("path=%s uid=%u, pid=%u", path, uid, pid);
  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Rm(path);

  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}


//------------------------------------------------------------------------------
// Rename file/dir
//------------------------------------------------------------------------------
int
xrd_rename (const char* oldpath,
            const char* newpath,
            uid_t uid,
            gid_t gid,
            pid_t pid)
{
  eos_static_info("oldpath=%s newpath=%s", oldpath, newpath, uid, pid);
  XrdCl::URL Url(xrd_user_url(uid, gid, pid));
  XrdCl::FileSystem fs(Url);
  XrdCl::XRootDStatus status = fs.Mv(oldpath, newpath);

  if (xrd_error_retc_map(status.errNo))
    return errno;
  else
    return 0;
}


//------------------------------------------------------------------------------
// Get user name from the uid and change the effective user ID of the thread
//------------------------------------------------------------------------------
const char*
xrd_mapuser (uid_t uid, gid_t gid, pid_t pid)
{
  eos_static_debug("uid=%lu gid=%lu pid=%lu",
                   (unsigned long) uid,
                   (unsigned long) gid,
                   (unsigned long) pid);

  XrdOucString sid = "";

  if (uid == 0)
  {
    uid = gid = 2;
  }

  char usergroup[16];
  // we user <hex-uid><hex-gid> as connection identifier
  snprintf(usergroup,sizeof(usergroup)-1,"%04x%04x", uid,gid);
  sid += usergroup;

  {
    XrdSysMutexHelper cLock(connectionIdMutex);
    if (connectionId)
    {
      sid = ".";
      sid += connectionId;
    }
  }

  return STRINGSTORE(sid.c_str());
}

//------------------------------------------------------------------------------
// Get a user private physical connection URL like root://<user>@<host>
// - if we are a user private mount we don't need to specify that
//------------------------------------------------------------------------------
const char*
xrd_user_url (uid_t uid,
              gid_t gid,
              pid_t pid)
{
  XrdOucString url = "root://";
  if (fuse_shared)
  {
    url += xrd_mapuser(uid, gid, pid);
    url += "@";
  }

  url += MgmHost.c_str();
  url += "/";

  eos_static_debug("uid=%lu gid=%lu pid=%lu url=%s",
                   (unsigned long) uid,
                   (unsigned long) gid,
                   (unsigned long) pid, url.c_str());
  return STRINGSTORE(url.c_str());
}

//------------------------------------------------------------------------------
// Init function
//------------------------------------------------------------------------------
void
xrd_init ()
{
  FILE* fstderr;

  // Open log file
  if (getuid())
  {
    fuse_shared = false; //eosfsd
    char logfile[1024];
    snprintf(logfile, sizeof ( logfile) - 1, "/tmp/eos-fuse.%d.log", getuid());

    // Running as a user ... we log into /tmp/eos-fuse.$UID.log
    if (!(fstderr = freopen(logfile, "a+", stderr)))
      fprintf(stderr, "error: cannot open log file %s\n", logfile);
    else
      chmod(logfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  }
  else
  {
    fuse_shared = true; //eosfsd

    // Running as root ... we log into /var/log/eos/fuse
    eos::common::Path cPath("/var/log/eos/fuse/fuse.log");
    cPath.MakeParentPath(S_IRWXU | S_IRGRP | S_IROTH);

    if (!(fstderr = freopen(cPath.GetPath(), "a+", stderr)))
      fprintf(stderr, "error: cannot open log file %s\n", cPath.GetPath());
    else
      chmod(cPath.GetPath(), S_IRUSR | S_IWUSR);
  }

  setvbuf(fstderr, (char*) NULL, _IONBF, 0);

  // Initialize hashes
  path2inode.set_empty_key("");
  path2inode.set_deleted_key("#__deleted__#");

  inode2path.set_empty_key(0);
  inode2path.set_deleted_key(0xffffffffll);

  dir2inodelist.set_empty_key(0);
  dir2inodelist.set_deleted_key(0xffffffffll);

  dir2dirbuf.set_empty_key(0);
  dir2dirbuf.set_deleted_key(0xffffffffll);

  inode2cache.set_empty_key(0);
  inode2cache.set_deleted_key(0xffffffffll);

  inodeuser2fd.set_empty_key("");
  inodeuser2fd.set_deleted_key("#__deleted__#");

  fd2fabst.set_empty_key(-1);
  fd2fabst.set_deleted_key(-2);

  // Create the root entry
  path2inode["/"] = 1;
  inode2path[1] = "/";
  eos::common::Mapping::VirtualIdentity_t vid;
  eos::common::Mapping::Root(vid);
  eos::common::Logging::Init();
  eos::common::Logging::SetUnit("FUSE@localhost");
  eos::common::Logging::gShortFormat = true;
  XrdOucString fusedebug = getenv("EOS_FUSE_DEBUG");

  if ((getenv("EOS_FUSE_DEBUG")) && (fusedebug != "0"))
  {
    eos::common::Logging::SetLogPriority(LOG_DEBUG);
  }
  else
  {
    if ((getenv("EOS_FUSE_LOGLEVEL")))
      eos::common::Logging::SetLogPriority(atoi(getenv("EOS_FUSE_LOGLEVEL")));
    else
      eos::common::Logging::SetLogPriority(LOG_INFO);
  }
  //............................................................................
  // Initialise the XrdClFileSystem object
  //............................................................................
  std::string address = getenv("EOS_RDRURL");
  if (address == "")
  {
    fprintf(stderr, "error: EOS_RDRURL is not defined so we fall back to  "
            "root://localhost:1094// \n");
    address = "root://localhost:1094//";
  }

  XrdCl::URL url(address);

  if (!url.IsValid())
  {
    eos_static_err("URL is not valid: %s", address.c_str());
    exit(-1);
  }

  MgmHost = address.c_str();
  MgmHost.replace("root://", "");

  // Check if we should set files executable
  if (getenv("EOS_FUSE_EXEC") && (!strcmp(getenv("EOS_FUSE_EXEC"), "1")))
    fuse_exec = true;

  // Initialise the XrdFileCache
  fuse_cache_read = false;
  fuse_cache_write = false;

  if ((!(getenv("EOS_FUSE_CACHE"))) || (getenv("EOS_FUSE_CACHE") && (!strcmp(getenv("EOS_FUSE_CACHE"), "0"))))
  {
    eos_static_notice("cache=false");
    XFC = NULL;
  }
  else
  {
    if (!getenv("EOS_FUSE_CACHE_SIZE"))
      setenv("EOS_FUSE_CACHE_SIZE", "30000000", 1); // ~300MB

    eos_static_notice("cache=true size=%s cache-read=%s cache-write=%s exec=%d",
                      getenv("EOS_FUSE_CACHE_SIZE"),
                      getenv("EOS_FUSE_CACHE_READ"),
                      getenv("EOS_FUSE_CACHE_WRITE"),
                      fuse_exec);

    XFC = FuseWriteCache::GetInstance(static_cast<size_t> (atol(getenv("EOS_FUSE_CACHE_SIZE"))));

    if (getenv("EOS_FUSE_CACHE_READ") && atoi(getenv("EOS_FUSE_CACHE_READ")))
      fuse_cache_read = true;

    if (getenv("EOS_FUSE_CACHE_WRITE") && atoi(getenv("EOS_FUSE_CACHE_WRITE")))
      fuse_cache_write = true;
  }

  passwdstore = new XrdOucHash<XrdOucString > ();
  stringstore = new XrdOucHash<XrdOucString > ();
}
