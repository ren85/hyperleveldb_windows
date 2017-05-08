// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <deque>

#ifdef WIN32
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <io.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>
#endif
#if defined(LEVELDB_PLATFORM_ANDROID)
#include <sys/stat.h>
#endif
#include "leveldb/env.h"
#include "leveldb/slice.h"

#ifdef WIN32
#include "util/win_logger.h"
#else
#include "util/posix_logger.h"
#endif
#include "port/port.h"
#include "util/logging.h"

#ifdef __linux
#include <sys/sysinfo.h>
#include <linux/unistd.h>
#endif

#include <fstream>

// Boost includes - see WINDOWS file to see which modules to install
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/thread/once.hpp>
#include <boost/thread/thread.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread/condition_variable.hpp>

namespace leveldb {
namespace {

// returns the ID of the current process
static boost::uint32_t current_process_id(void) {
#ifdef _WIN32
  return static_cast<boost::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<boost::uint32_t>(::getpid());
#endif
}

// returns the ID of the current thread
static boost::uint32_t current_thread_id(void) {
#ifdef _WIN32
  return static_cast<boost::uint32_t>(::GetCurrentThreadId());
#else
#ifdef __linux
  return static_cast<boost::uint32_t>(::syscall(__NR_gettid));
#else
  // just return the pid
  return current_process_id();
#endif
#endif
}

static char global_read_only_buf[0x8000];

class PosixSequentialFile: public SequentialFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  PosixSequentialFile(const std::string& fname, FILE* f)
    : filename_(fname), file_(f) { }
  virtual ~PosixSequentialFile() { fclose(file_); }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
  Status s;
#ifdef BSD
  // fread_unlocked doesn't exist on FreeBSD
  size_t r = fread(scratch, 1, n, file_);
#else
  size_t r = fread_unlocked(scratch, 1, n, file_);
#endif
  *result = Slice(scratch, r);
  if (r < n) {
    if (feof(file_)) {
    // We leave status as ok if we hit the end of the file
    } else {
    // A partial read with an error: return a non-ok status
    s = Status::IOError(filename_, strerror(errno));
    }
  }
  return s;
  }

  virtual Status Skip(uint64_t n) {
  if (fseek(file_, n, SEEK_CUR)) {
    return Status::IOError(filename_, strerror(errno));
  }
  return Status::OK();
  }
};

// We preallocate up to an extra megabyte and use memcpy to append new
// data to the file.  This is safe since we either properly close the
// file before reading from it, or for log files, the reading code
// knows enough to skip zero suffixes.
class PosixMmapFile : public ConcurrentWritableFile {
private:
	PosixMmapFile(const PosixMmapFile&);
	PosixMmapFile& operator = (const PosixMmapFile&);
	struct MmapSegment {
		char* base_;
		HANDLE handle;
	};

	std::string filename_;    // Path to the file
	int fd_;                  // The open file
	const size_t block_size_; // System page size
	uint64_t end_offset_;     // Where does the file end?
	MmapSegment* segments_;   // mmap'ed regions of memory
	size_t segments_sz_;      // number of segments that are truncated
	bool trunc_in_progress_;  // is there an ongoing truncate operation?
	uint64_t trunc_waiters_;  // number of threads waiting for truncate
	port::Mutex mtx_;         // Protection for state
	port::CondVar cnd_;       // Wait for truncate
	HANDLE _base_handle;

							  // Roundup x to a multiple of y
	static size_t Roundup(size_t x, size_t y) {
		return ((x + y - 1) / y) * y;
	}

	bool GrowViaTruncate(uint64_t block) {
		mtx_.Lock();
		while (trunc_in_progress_ && segments_sz_ <= block) {
			++trunc_waiters_;
			cnd_.Wait();
			--trunc_waiters_;
		}
		uint64_t cur_sz = segments_sz_;
		trunc_in_progress_ = cur_sz <= block;
		mtx_.Unlock();

		bool error = false;
		if (cur_sz <= block) {
			uint64_t new_sz = ((block + 7) & ~7ULL) + 1;
			/*if (ftruncate(fd_, new_sz * block_size_) < 0) {
				error = true;
			}*/
			if (_chsize(fd_, new_sz * block_size_) < 0) {
				error = true;
			}
			MmapSegment* new_segs = new MmapSegment[new_sz];
			MmapSegment* old_segs = NULL;
			mtx_.Lock();
			old_segs = segments_;
			for (size_t i = 0; i < segments_sz_; ++i) {
				new_segs[i].base_ = old_segs[i].base_;
				new_segs[i].handle = old_segs[i].handle;
			}
			for (size_t i = segments_sz_; i < new_sz; ++i) {
				new_segs[i].base_ = NULL;
				new_segs[i].handle = NULL;
			}
			segments_ = new_segs;
			segments_sz_ = new_sz;
			trunc_in_progress_ = false;
			cnd_.SignalAll();
			mtx_.Unlock();
			delete[] old_segs;
		}
		return !error;
	}

	bool UnmapSegment(char* base) {
		int reval = UnmapViewOfFile(base);
		CloseHandle(_base_handle);
		return reval != 0;//munmap(base, block_size_) >= 0;
	}

	// Call holding mtx_
	char* GetSegment(uint64_t block) {
		char* base = NULL;
		mtx_.Lock();
		size_t cur_sz = segments_sz_;
		if (block < segments_sz_) {
			base = segments_[block].base_;
		}
		mtx_.Unlock();
		if (base) {
			return base;
		}
		if (cur_sz <= block) {
			if (!GrowViaTruncate(block)) {
				return NULL;
			}
		}
		/*void* ptr = mmap(NULL, block_size_, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd_, block * block_size_);
		if (ptr == MAP_FAILED) {
			abort();
			return NULL;
		}*/

		void* ptr = NULL;
		DWORD off_hi = (DWORD)((block * block_size_) >> 32);
		DWORD off_lo = (DWORD)((block * block_size_) & 0xFFFFFFFF);
		HANDLE _hFile = (HANDLE)_get_osfhandle(fd_);
		_base_handle = CreateFileMappingA(
			_hFile,
			NULL,
			PAGE_READWRITE,
			0,
			0,
			0);
		if (_base_handle != NULL) {
			ptr = (char*)MapViewOfFile(_base_handle,
				FILE_MAP_ALL_ACCESS,
				off_hi,
				off_lo,
				block_size_);
			if (ptr == NULL)
			{
				abort();
				return NULL;
			}
		}
		else
		{
			abort();
			return NULL;
		}

		bool unmap = false;
		mtx_.Lock();
		assert(block < segments_sz_);
		if (segments_[block].base_) {
			base = segments_[block].base_;
			unmap = true;
		}
		else {
			base = reinterpret_cast<char*>(ptr);
			segments_[block].base_ = base;
			segments_[block].handle = _base_handle;
			unmap = false;
		}
		mtx_.Unlock();
		if (unmap) {
			if (!UnmapSegment(reinterpret_cast<char*>(ptr))) {
				return NULL;
			}
		}
		return base;
	}

public:
	PosixMmapFile(const std::string& fname, int fd, size_t page_size)
		: filename_(fname),
		fd_(fd),
		block_size_(Roundup(page_size, 262144)),
		end_offset_(0),
		segments_(NULL),
		segments_sz_(0),
		trunc_in_progress_(false),
		trunc_waiters_(0),
		mtx_(),
		cnd_(&mtx_) {
		assert((page_size & (page_size - 1)) == 0);
	}

	~PosixMmapFile() {
		PosixMmapFile::Close();
	}

	virtual Status WriteAt(uint64_t offset, const Slice& data) {
		const uint64_t end = offset + data.size();
		const char* src = data.data();
		uint64_t rem = data.size();
		mtx_.Lock();
		end_offset_ = end_offset_ < end ? end : end_offset_;
		mtx_.Unlock();
		while (rem > 0) {
			const uint64_t block = offset / block_size_;
			char* base = GetSegment(block);
			if (!base) {
				return Status::IOError(filename_, "write at");
			}
			const uint64_t loff = offset - block * block_size_;
			uint64_t n = block_size_ - loff;
			n = n < rem ? n : rem;
			memmove(base + loff, src, n);
			rem -= n;
			src += n;
			offset += n;
		}
		return Status::OK();
	}

	virtual Status Append(const Slice& data) {
		mtx_.Lock();
		uint64_t offset = end_offset_;
		mtx_.Unlock();
		return WriteAt(offset, data);
	}

	virtual Status Close() {
		Status s;
		int fd;
		MmapSegment* segments;
		size_t end_offset;
		size_t segments_sz;
		mtx_.Lock();
		fd = fd_;
		fd_ = -1;
		end_offset = end_offset_;
		end_offset_ = 0;
		segments = segments_;
		segments_ = NULL;
		segments_sz = segments_sz_;
		segments_sz_ = 0;
		mtx_.Unlock();
		if (fd < 0) {
			return s;
		}
		/*for (size_t i = 0; i < segments_sz; ++i) {
			if (segments[i].base_ != NULL &&
				munmap(segments[i].base_, block_size_) < 0) {
				s = IOError(filename_, errno);
			}
		}
		delete[] segments;
		if (ftruncate(fd, end_offset) < 0) {
			s = IOError(filename_, errno);
		}
		if (close(fd) < 0) {
			if (s.ok()) {
				s = IOError(filename_, errno);
			}
		}*/

		for (size_t i = 0; i < segments_sz; ++i) {
			if (segments[i].base_ != NULL)
			{
				int reval = UnmapViewOfFile(segments[i].base_);
				CloseHandle(segments[i].handle);
				if (reval == 0)
				{
					s = Status::IOError(filename_, "bad close 1");
				}
			}
		}
		delete[] segments;
		if (_chsize(fd, end_offset) < 0) {
			s = Status::IOError(filename_, "bad close 2");
		}
		if (close(fd) < 0) {
			if (s.ok()) {
				s = Status::IOError(filename_, "bad close 3");
			}
		}


		return s;
	}

	Status Flush() {
		return Status::OK();
	}

	Status SyncDirIfManifest() {
		const char* f = filename_.c_str();
		const char* sep = strrchr(f, '/');
		Slice basename;
		std::string dir;
		if (sep == NULL) {
			dir = ".";
			basename = f;
		}
		else {
			dir = std::string(f, sep - f);
			basename = sep + 1;
		}
		Status s;
		if (basename.starts_with("MANIFEST")) {
			int fd = open(dir.c_str(), O_RDONLY);
			/*if (fd < 0) {
				s = IOError(dir, errno);
			}
			else {
				if (fsync(fd) < 0) {
					s = IOError(dir, errno);
				}
				close(fd);
			}*/
			if (fd < 0) {
				s = Status::IOError(dir, "sync error 1");
			}
			else {
				HANDLE _hFile = (HANDLE)_get_osfhandle(fd);
				if (FlushFileBuffers(_hFile) == 0) {					
					s = Status::IOError(dir, "sync error 2");
				}
				close(fd);
			}
		}
		return s;
	}

	virtual Status Sync() {
		// Ensure new files referred to by the manifest are in the filesystem.
		Status s;
		/*Status s = SyncDirIfManifest();

		if (!s.ok()) {
			return s;
		}*/

		size_t block = 0;
		while (true) {
			char* base = NULL;
			mtx_.Lock();
			if (block < segments_sz_) {
				base = segments_[block].base_;
			}
			mtx_.Unlock();
			if (!base) {
				break;
			}
			/*if (msync(base, block_size_, MS_SYNC) < 0) {
				s = IOError(filename_, errno);
			}*/
			if (!FlushViewOfFile(base, block_size_)) {
				s = Status::IOError(filename_, "flush error");
			}
			if (!FlushFileBuffers(segments_[block].handle) == 0) {
				s = Status::IOError(filename_, "flush error 2");
			}
			++block;
		}
		return s;
	}
};
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);               \
  void operator=(const TypeName&)

class PosixRandomAccessFile: public RandomAccessFile {
// private:
//  std::string filename_;
//  int fd_;
//  mutable boost::mutex mu_;
//
// public:
//  PosixRandomAccessFile(const std::string& fname, int fd)
//    : filename_(fname), fd_(fd) { }
//  virtual ~PosixRandomAccessFile() { close(fd_); }
//
//  virtual Status Read(uint64_t offset, size_t n, Slice* result,
//            char* scratch) const {
//    Status s;
//#ifdef WIN32
//    // no pread on Windows so we emulate it with a mutex
//    boost::unique_lock<boost::mutex> lock(mu_);
//
//    if (::_lseeki64(fd_, offset, SEEK_SET) == -1L) {
//      return Status::IOError(filename_, strerror(errno));
//    }
//
//    int r = ::_read(fd_, scratch, n);
//    *result = Slice(scratch, (r < 0) ? 0 : r);
//    lock.unlock();
//#else
//    ssize_t r = pread(fd_, scratch, n, static_cast<off_t>(offset));
//    *result = Slice(scratch, (r < 0) ? 0 : r);
//#endif
//    if (r < 0) {
//      // An error: return a non-ok status
//      s = Status::IOError(filename_, strerror(errno));
//    }
//    return s;
//  }
public:
	friend class PosixEnv;
	virtual ~PosixRandomAccessFile();
	virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const;
	BOOL isEnable();
private:
	BOOL _Init(LPCWSTR path);
	void _CleanUp();
	PosixRandomAccessFile(const std::string& fname);
	HANDLE _hFile;
	const std::string _filename;
	DISALLOW_COPY_AND_ASSIGN(PosixRandomAccessFile);
};

void ToWidePath(const std::string& value, std::wstring& target) {
	wchar_t buffer[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH);
	target = buffer;
}

void ToNarrowPath(const std::wstring& value, std::string& target) {
	char buffer[MAX_PATH];
	WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH, NULL, NULL);
	target = buffer;
}

PosixRandomAccessFile::PosixRandomAccessFile(const std::string& fname) :
	_filename(fname), _hFile(NULL)
{
	std::wstring path;
	ToWidePath(fname, path);
	_Init(path.c_str());
}

PosixRandomAccessFile::~PosixRandomAccessFile()
{
	_CleanUp();
}

Status PosixRandomAccessFile::Read(uint64_t offset, size_t n, Slice* result, char* scratch) const
{
	Status sRet;
	OVERLAPPED ol = { 0 };
	ZeroMemory(&ol, sizeof(ol));
	ol.Offset = (DWORD)offset;
	ol.OffsetHigh = (DWORD)(offset >> 32);
	DWORD hasRead = 0;
	if (!ReadFile(_hFile, scratch, n, &hasRead, &ol))
		sRet = Status::IOError(_filename, /*Win32::GetLastErrSz()*/"error read random file");
	else
		*result = Slice(scratch, hasRead);
	return sRet;
}

BOOL PosixRandomAccessFile::_Init(LPCWSTR path)
{
	BOOL bRet = FALSE;
	if (!_hFile)
		_hFile = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, NULL);
	if (!_hFile || _hFile == INVALID_HANDLE_VALUE)
		_hFile = NULL;
	else
		bRet = TRUE;
	return bRet;
}

BOOL PosixRandomAccessFile::isEnable()
{
	return _hFile ? TRUE : FALSE;
}

void PosixRandomAccessFile::_CleanUp()
{
	if (_hFile) {
		::CloseHandle(_hFile);
		_hFile = NULL;
	}
}


// We preallocate up to an extra megabyte and use memcpy to append new
// data to the file.  This is safe since we either properly close the
// file before reading from it, or for log files, the reading code
// knows enough to skip zero suffixes.

class BoostFile : public WritableFile {

public:
  explicit BoostFile(std::string path) : path_(path), written_(0) {
    Open();
  }

  virtual ~BoostFile() {
    Close();
  }

private:
  void Open() {
    // we truncate the file as implemented in env_posix
     file_.open(path_.generic_string().c_str(), 
         std::ios_base::trunc | std::ios_base::out | std::ios_base::binary);
     written_ = 0;
  }

public:
  virtual Status Append(const Slice& data) {
    Status result;
    file_.write(data.data(), data.size());
    if (!file_.good()) {
      result = Status::IOError(
          path_.generic_string() + " Append", "cannot write");
    }
    return result;
  }

  virtual Status Close() {
    Status result;

    try {
      if (file_.is_open()) {
        Sync();
        file_.close();
      }
    } catch (const std::exception & e) {
      result = Status::IOError(path_.generic_string() + " close", e.what());
    }

    return result;
  }

  virtual Status Flush() {
    file_.flush();
    return Status::OK();
  }

  virtual Status Sync() {
    Status result;
    try {
      Flush();
    } catch (const std::exception & e) {
      result = Status::IOError(path_.string() + " sync", e.what());
    }

    return result;
  }

private:
  boost::filesystem::path path_;
  boost::uint64_t written_;
  std::ofstream file_;
};



class BoostFileLock : public FileLock {
 public:
  boost::interprocess::file_lock fl_;
};

//void ToWidePath(const std::string& value, std::wstring& target) {
//	wchar_t buffer[MAX_PATH];
//	MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH);
//	target = buffer;
//}
//
//void ToNarrowPath(const std::wstring& value, std::string& target) {
//	char buffer[MAX_PATH];
//	WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH, NULL, NULL);
//	target = buffer;
//}

std::string GetCurrentDir();
static const std::string CurrentDir = GetCurrentDir();
std::string GetCurrentDir()
{
	CHAR path[MAX_PATH];
	::GetModuleFileNameA(::GetModuleHandleA(NULL), path, MAX_PATH);
	*strrchr(path, '\\') = 0;
	return std::string(path);
}
class PosixEnv : public Env {
 public:
  PosixEnv();
  virtual ~PosixEnv() {
    fprintf(stderr, "Destroying Env::Default()\n");
    exit(1);
  }

  virtual Status NewSequentialFile(const std::string& fname,
                   SequentialFile** result) {
    FILE* f = fopen(fname.c_str(), "rb");
    if (f == NULL) {
      *result = NULL;
      return Status::IOError(fname, strerror(errno));
    } else {
      *result = new PosixSequentialFile(fname, f);
      return Status::OK();
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                   RandomAccessFile** result) {
//#ifdef WIN32
//    int fd = _open(fname.c_str(), _O_RDONLY | _O_RANDOM | _O_BINARY);
//#else
//    int fd = open(fname.c_str(), O_RDONLY);
//#endif
//    if (fd < 0) {
//      *result = NULL;
//      return Status::IOError(fname, strerror(errno));
//    }
//    *result = new PosixRandomAccessFile(fname, fd);
//    return Status::OK();
	  Status sRet;
	  std::string path = fname;
	  PosixRandomAccessFile* pFile = new PosixRandomAccessFile(ModifyPath(path));
	  if (!pFile->isEnable()) {
		  delete pFile;
		  *result = NULL;
		  sRet = Status::IOError(path, "Could not create random access file.");
	  }
	  else
		  *result = pFile;
	  return sRet;
  }

  //void ToNarrowPath(const std::wstring& value, std::string& target) {
	 // char buffer[MAX_PATH];
	 // WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH, NULL, NULL);
	 // target = buffer;
  //}
  //std::string GetLastErrSz()
  //{
	 // LPWSTR lpMsgBuf;
	 // FormatMessageW(
		//  FORMAT_MESSAGE_ALLOCATE_BUFFER |
		//  FORMAT_MESSAGE_FROM_SYSTEM |
		//  FORMAT_MESSAGE_IGNORE_INSERTS,
		//  NULL,
		//  GetLastError(),
		//  0, // Default language
		//  (LPWSTR)&lpMsgBuf,
		//  0,
		//  NULL
	 // );
	 // std::string Err;
	 // ToNarrowPath(lpMsgBuf, Err);
	 // LocalFree(lpMsgBuf);
	 // return Err;
  //}

  void ToWidePath(const std::string& value, std::wstring& target) {
	  wchar_t buffer[MAX_PATH];
	  MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH);
	  target = buffer;
  }

  void ToNarrowPath(const std::wstring& value, std::string& target) {
	  char buffer[MAX_PATH];
	  WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, buffer, MAX_PATH, NULL, NULL);
	  target = buffer;
  }
  std::string& ModifyPath(std::string& path)
  {
	  if (path[0] == '/' || path[0] == '\\') {
		  path = CurrentDir + path;
	  }
	  std::replace(path.begin(), path.end(), '/', '\\');

	  return path;
  }
  virtual Status NewWritableFile(const std::string& fname,
                 WritableFile** result) {
    //Status s;
    //try {
    //  // will create a new empty file to write to
    //  *result = new BoostFile(fname);
    //}
    //catch (const std::exception & e) {
    //  s = Status::IOError(fname, e.what());
    //}
	//return s;

	Status s;
	//const int fd = _open(fname.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
	std::wstring path;
	ToWidePath(fname.c_str(), path);
	DWORD Flag = boost::filesystem::exists(fname) ? OPEN_EXISTING : CREATE_ALWAYS;
	HANDLE _hFile = CreateFileW(path.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
		NULL,
		Flag,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (!_hFile)
	{
		*result = NULL;
		s = Status::IOError(fname, "bad open 2: ");
	}
	else
	{
		int fd = _open_osfhandle(reinterpret_cast<intptr_t>(_hFile), 0644);
		if (fd < 0) {
			*result = NULL;
			s = Status::IOError(fname, "bad open 2: ");
		}
		else {
			*result = new PosixMmapFile(fname, fd, getpagesize());
		}
	}
	return s;
  }

  virtual bool FileExists(const std::string& fname) {
    return boost::filesystem::exists(fname);
  }

  virtual Status GetChildren(const std::string& dir,
               std::vector<std::string>* result) {
    result->clear();

    boost::system::error_code ec;
    boost::filesystem::directory_iterator current(dir, ec);
    if (ec) {
      return Status::IOError(dir, ec.message());
    }

    boost::filesystem::directory_iterator end;

    for(; current != end; ++current) {
      result->push_back(current->path().filename().generic_string());
    }

    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    boost::system::error_code ec;

    boost::filesystem::remove(fname, ec);

    Status result;

    if (ec) {
      result = Status::IOError(fname, ec.message());
    }

    return result;
  }

  virtual Status CreateDir(const std::string& name) {
      Status result;

      if (boost::filesystem::exists(name) &&
          boost::filesystem::is_directory(name)) {
        return result;
      }

      boost::system::error_code ec;

      if (!boost::filesystem::create_directories(name, ec)) {
        result = Status::IOError(name, ec.message());
      }

      return result;
    };

    virtual Status DeleteDir(const std::string& name) {
    Status result;

    boost::system::error_code ec;
    if (!boost::filesystem::remove_all(name, ec)) {
      result = Status::IOError(name, ec.message());
    }

    return result;
  };

  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    boost::system::error_code ec;

    Status result;

    *size = static_cast<uint64_t>(boost::filesystem::file_size(fname, ec));
    if (ec) {
      *size = 0;
       result = Status::IOError(fname, ec.message());
    }

    return result;
  }


  virtual Status LinkFile(const std::string& src, const std::string& target) {
	  //Status result;
	  //if (link(src.c_str(), target.c_str()) != 0) {
		 // result = Status::IOError(src, ec.message());
	  //}
	  //return result;

	  boost::system::error_code ec;

	  boost::filesystem::create_symlink(src, target, ec);

	  Status result;

	  if (ec) {
		  result = Status::IOError(src, ec.message());
	  }

	  return result;
  }

  virtual Status CopyFile(const std::string& src, const std::string& target) {
	  //Status result;
	  //if (link(src.c_str(), target.c_str()) != 0) {
	  // result = Status::IOError(src, ec.message());
	  //}
	  //return result;

	  boost::system::error_code ec;

	  boost::filesystem::copy_file(src, target, ec);

	  Status result;

	  if (ec) {
		  result = Status::IOError(src, ec.message());
	  }

	  return result;
  }
  int getpagesize(void)
  {
	  SYSTEM_INFO system_info;
	  GetSystemInfo(&system_info);
	  return system_info.dwPageSize;
  }
  virtual Status NewConcurrentWritableFile(const std::string& fname, ConcurrentWritableFile** result) {
	  Status s;
	  const int fd = _open(fname.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
	  if (fd < 0) {
		  *result = NULL;
		  s = Status::IOError(fname, "bad open");
	  }
	  else {
		  *result = new PosixMmapFile(fname, fd, getpagesize());
	  }
	  return s;
  
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) {
    //boost::system::error_code ec;

    //boost::filesystem:: rename(src, target, ec);

    //Status result;

    //if (ec) {
    //  result = Status::IOError(src, ec.message());
    //}

    //return result;
	  Status sRet;
	  std::string src_path = src;
	  std::wstring wsrc_path;
	  ToWidePath(ModifyPath(src_path), wsrc_path);
	  std::string target_path = target;
	  std::wstring wtarget_path;
	  ToWidePath(ModifyPath(target_path), wtarget_path);

	  if (!MoveFileW(wsrc_path.c_str(), wtarget_path.c_str())) {
		  DWORD err = GetLastError();
		  if (err == 0x000000b7) {
			  if (!::DeleteFileW(wtarget_path.c_str()))
				  sRet = Status::IOError(src, "Could not rename file.");
			  else if (!::MoveFileW(wsrc_path.c_str(),
				  wtarget_path.c_str()))
				  sRet = Status::IOError(src, "Could not rename file.");
		  }
	  }
	  return sRet;
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = NULL;

    Status result;

    try {
      if (!boost::filesystem::exists(fname)) {
        std::ofstream of(fname, std::ios_base::trunc | std::ios_base::out);
      }

      assert(boost::filesystem::exists(fname));

      boost::interprocess::file_lock fl(fname.c_str());
      BoostFileLock * my_lock = new BoostFileLock();
      my_lock->fl_ = std::move(fl);
      my_lock->fl_.lock();
      *lock = my_lock;
    } catch (const std::exception & e) {
      result = Status::IOError("lock " + fname, e.what());
    }

    return result;
  }

  virtual Status UnlockFile(FileLock* lock) {

    Status result;

    try {
      BoostFileLock * my_lock = static_cast<BoostFileLock *>(lock);
      my_lock->fl_.unlock();
      delete my_lock;
    } catch (const std::exception & e) {
      result = Status::IOError("unlock", e.what());
    }

    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    boost::system::error_code ec;
    boost::filesystem::path temp_dir = 
        boost::filesystem::temp_directory_path(ec);
    if (ec) {
      temp_dir = "tmp";
    }

    temp_dir /= "leveldb_tests";
    temp_dir /= boost::lexical_cast<std::string>(current_process_id());

    // Directory may already exist
    CreateDir(temp_dir.generic_string());

    *result = temp_dir.generic_string();

    return Status::OK();
  }

#ifndef WIN32
  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }
#endif

  virtual Status NewLogger(const std::string& fname, Logger** result) {
  FILE* f = fopen(fname.c_str(), "wt");
  if (f == NULL) {
    *result = NULL;
    return Status::IOError(fname, strerror(errno));
  } else {
#ifdef WIN32
    *result = new WinLogger(f);
#else
    *result = new PosixLogger(f, &PosixEnv::gettid);
#endif
    return Status::OK();
  }
  }

  virtual uint64_t NowMicros() {
    return static_cast<uint64_t>(
        boost::posix_time::microsec_clock::universal_time()
        .time_of_day().total_microseconds());
  }

  virtual void SleepForMicroseconds(int micros) {
  boost::this_thread::sleep(boost::posix_time::microseconds(micros));
  }

 private:
  void PthreadCall(const char* label, int result) {
  if (result != 0) {
    fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
    exit(1);
  }
  }

  // BGThread() is the body of the background thread
  void BGThread();

  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<PosixEnv*>(arg)->BGThread();
    return NULL;
  }

  boost::mutex mu_;
  boost::condition_variable bgsignal_;
  boost::scoped_ptr<boost::thread> bgthread_;

  // Entry per Schedule() call
  struct BGItem { void* arg; void (*function)(void*); };
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;
};

PosixEnv::PosixEnv() { }

void PosixEnv::Schedule(void (*function)(void*), void* arg) {
  boost::unique_lock<boost::mutex> lock(mu_);

  // Start background thread if necessary
  if (!bgthread_) {
     bgthread_.reset(
         new boost::thread(boost::bind(&PosixEnv::BGThreadWrapper, this)));
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  lock.unlock();

  bgsignal_.notify_one();

}

void PosixEnv::BGThread() {
  while (true) {
  // Wait until there is an item that is ready to run
  boost::unique_lock<boost::mutex> lock(mu_);

  while (queue_.empty()) {
    bgsignal_.wait(lock);
  }

  void (*function)(void*) = queue_.front().function;
  void* arg = queue_.front().arg;
  queue_.pop_front();

  lock.unlock();
  (*function)(arg);
  }
}

namespace {
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};
}

static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return NULL;
}

void PosixEnv::StartThread(void (*function)(void* arg), void* arg) {
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;

  boost::thread t(boost::bind(&StartThreadWrapper, state));
}

}

static boost::once_flag once = BOOST_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { 
  ::memset(global_read_only_buf, 0, sizeof(global_read_only_buf));
  default_env = new PosixEnv;
}

Env* Env::Default() {
  boost::call_once(once, InitDefaultEnv);

  return default_env;
}

}
