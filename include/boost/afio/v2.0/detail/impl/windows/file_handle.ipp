/* file_handle.hpp
A handle to a file
(C) 2015 Niall Douglas http://www.nedprod.com/
File Created: Dec 2015


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#include "../../../file_handle.hpp"
#include "../../../stat.hpp"
#include "import.hpp"

BOOST_AFIO_V2_NAMESPACE_BEGIN

// allocate at process start to ensure later failure to allocate won't cause failure
static fixme_path temporary_files_directory_("C:\\no_temporary_directories_accessible");
const fixme_path &fixme_temporary_files_directory() noexcept
{
  static struct temporary_files_directory_done_
  {
    temporary_files_directory_done_()
    {
      try
      {
        fixme_path::string_type buffer;
        auto testpath = [&]() {
          size_t len = buffer.size();
          if(buffer[len - 1] == '\\')
            buffer.resize(--len);
          buffer.append(L"\\afio_tempfile_probe_file.tmp");
          HANDLE h = CreateFile(buffer.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
          if(INVALID_HANDLE_VALUE != h)
          {
            CloseHandle(h);
            buffer.resize(len);
            temporary_files_directory_ = std::move(buffer);
            return true;
          }
          return false;
        };
        {
          error_code ec;
          // Try filesystem::temp_directory_path() before all else. This probably just calls GetTempPath().
          buffer = stl1z::filesystem::temp_directory_path(ec);
          if(!ec && testpath())
            return;
        }
        // GetTempPath() returns one of (in order): %TMP%, %TEMP%, %USERPROFILE%, GetWindowsDirectory()\Temp
        static const wchar_t *variables[] = {L"TMP", L"TEMP", L"USERPROFILE"};
        for(size_t n = 0; n < sizeof(variables) / sizeof(variables[0]); n++)
        {
          buffer.resize(32768);
          DWORD len = GetEnvironmentVariable(variables[n], (LPWSTR) buffer.data(), (DWORD) buffer.size());
          if(len && len < buffer.size())
          {
            buffer.resize(len);
            if(variables[n][0] == 'U')
            {
              buffer.append(L"\\AppData\\Local\\Temp");
              if(testpath())
                return;
              buffer.resize(len);
              buffer.append(L"\\Local Settings\\Temp");
              if(testpath())
                return;
              buffer.resize(len);
            }
            if(testpath())
              return;
          }
        }
        // Finally if everything earlier failed e.g. if our environment block is zeroed,
        // fall back to Win3.1 era "the Windows directory" which definitely won't be
        // C:\Windows nowadays
        buffer.resize(32768);
        DWORD len = GetWindowsDirectory((LPWSTR) buffer.data(), (UINT) buffer.size());
        if(len && len < buffer.size())
        {
          buffer.resize(len);
          buffer.append(L"\\Temp");
          if(testpath())
            return;
        }
      }
      catch(...)
      {
      }
    }
  } init;
  return temporary_files_directory_;
}

result<void> file_handle::_fetch_inode() noexcept
{
  stat_t s;
  BOOST_OUTCOME_TRYV(s.fill(*this, stat_t::want::dev|stat_t::want::ino));
  _devid = s.st_dev;
  _inode = s.st_ino;
  return make_valued_result<void>();
}

result<file_handle> file_handle::file(file_handle::path_type _path, file_handle::mode _mode, file_handle::creation _creation, file_handle::caching _caching, file_handle::flag flags) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  result<file_handle> ret(file_handle(std::move(_path), native_handle_type(), _caching, flags));
  native_handle_type &nativeh = ret.get()._v;
  BOOST_OUTCOME_TRY(access, access_mask_from_handle_mode(nativeh, _mode));
  DWORD creation = OPEN_EXISTING;
  switch(_creation)
  {
  case creation::open_existing:
    break;
  case creation::only_if_not_exist:
    creation = CREATE_NEW;
    break;
  case creation::if_needed:
    creation = OPEN_ALWAYS;
    break;
  case creation::truncate:
    creation = TRUNCATE_EXISTING;
    break;
  }
  BOOST_OUTCOME_TRY(attribs, attributes_from_handle_caching_and_flags(nativeh, _caching, flags));
  nativeh.behaviour |= native_handle_type::disposition::file;
  if(INVALID_HANDLE_VALUE == (nativeh.h = CreateFileW_(ret.value()._path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, creation, attribs, NULL)))
  {
    DWORD errcode = GetLastError();
    // assert(false);
    BOOST_AFIO_LOG_FUNCTION_CALL(0);
    return make_errored_result<file_handle>(errcode, last190(ret.value()._path.u8string()));
  }
  BOOST_AFIO_LOG_FUNCTION_CALL(nativeh.h);
  if(!(flags & disable_safety_unlinks))
  {
    BOOST_OUTCOME_TRYV(_fetch_inode());
  }
  if(flags & flag::unlink_on_close)
  {
    // Hide this item
    IO_STATUS_BLOCK isb = make_iostatus();
    FILE_BASIC_INFORMATION fbi;
    memset(&fbi, 0, sizeof(fbi));
    fbi.FileAttributes = FILE_ATTRIBUTE_HIDDEN;
    NtSetInformationFile(nativeh.h, &isb, &fbi, sizeof(fbi), FileBasicInformation);
    if(flags & flag::overlapped)
      ntwait(nativeh.h, isb, deadline());
  }
  if(_creation == creation::truncate && ret.value().are_safety_fsyncs_issued())
    FlushFileBuffers(nativeh.h);
  return ret;
}

result<file_handle> file_handle::temp_inode(path_type dirpath, mode _mode, flag flags) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  caching _caching = caching::temporary;
  // No need to rename to random on unlink or check inode before unlink
  flags |= flag::unlink_on_close | flag::disable_safety_unlinks | flag::win_disable_unlink_emulation;
  result<file_handle> ret(file_handle(path_type(), native_handle_type(), _caching, flags));
  native_handle_type &nativeh = ret.get()._v;
  BOOST_OUTCOME_TRY(access, access_mask_from_handle_mode(nativeh, _mode));
  BOOST_OUTCOME_TRY(attribs, attributes_from_handle_caching_and_flags(nativeh, _caching, flags));
  nativeh.behaviour |= native_handle_type::disposition::file;
  DWORD creation = CREATE_NEW;
  for(;;)
  {
    try
    {
      ret.value()._path = dirpath / (utils::random_string(32) + ".tmp");
    }
    BOOST_OUTCOME_CATCH_ALL_EXCEPTION_TO_RESULT
    if(INVALID_HANDLE_VALUE == (nativeh.h = CreateFileW_(ret.value()._path.c_str(), access, /* no read nor write access for others */ FILE_SHARE_DELETE, NULL, creation, attribs, NULL)))
    {
      DWORD errcode = GetLastError();
      if(ERROR_FILE_EXISTS == errcode)
        continue;
      BOOST_AFIO_LOG_FUNCTION_CALL(0);
      return make_errored_result<file_handle>(errcode, last190(ret.value()._path.u8string()));
    }
    BOOST_AFIO_LOG_FUNCTION_CALL(nativeh.h);
    BOOST_OUTCOME_TRYV(_fetch_inode());  // It can be useful to know the inode of temporary inodes
    if(nativeh.h)
    {
      // Hide this item
      IO_STATUS_BLOCK isb = make_iostatus();
      FILE_BASIC_INFORMATION fbi;
      memset(&fbi, 0, sizeof(fbi));
      fbi.FileAttributes = FILE_ATTRIBUTE_HIDDEN;
      NtSetInformationFile(nativeh.h, &isb, &fbi, sizeof(fbi), FileBasicInformation);
    }
    return ret;
  }
}


result<file_handle> file_handle::clone() const noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  result<file_handle> ret(file_handle(native_handle_type(), _path, _devid, _inode, _caching, _flags));
  ret.value()._v.behaviour = _v.behaviour;
  if(!DuplicateHandle(GetCurrentProcess(), _v.h, GetCurrentProcess(), &ret.value()._v.h, 0, false, DUPLICATE_SAME_ACCESS))
    return make_errored_result<file_handle>(GetLastError());
  return ret;
}

result<file_handle::path_type> file_handle::relink(path_type newpath) noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  if(newpath.is_relative())
    newpath = _path.parent_path() / newpath;

  // FIXME: As soon as we implement fat paths, eliminate this mallocing NT path conversion nonsense
  UNICODE_STRING NtPath;
  if(!RtlDosPathNameToNtPathName_U(newpath.c_str(), &NtPath, NULL, NULL))
    return make_errored_result<path_type>(stl11::errc::no_such_file_or_directory);
  auto unntpath = undoer([&NtPath] {
    if(!HeapFree(GetProcessHeap(), 0, NtPath.Buffer))
      abort();
  });

  IO_STATUS_BLOCK isb = make_iostatus();
  alignas(8) fixme_path::value_type buffer[32769];
  FILE_RENAME_INFORMATION *fni = (FILE_RENAME_INFORMATION *) buffer;
  fni->ReplaceIfExists = true;
  // fni->RootDirectory = newdirh ? newdirh->native_handle() : nullptr;
  fni->RootDirectory = nullptr;
  fni->FileNameLength = NtPath.Length;
  memcpy(fni->FileName, NtPath.Buffer, fni->FileNameLength);
  NtSetInformationFile(_v.h, &isb, fni, sizeof(FILE_RENAME_INFORMATION) + fni->FileNameLength, FileRenameInformation);
  if(_flags & flag::overlapped)
    ntwait(_v.h, isb, deadline());
  if(STATUS_SUCCESS != isb.Status)
    return make_errored_result_nt<path_type>(isb.Status);
  _path = std::move(newpath);
  return _path;
}

result<void> file_handle::unlink() noexcept
{
  windows_nt_kernel::init();
  using namespace windows_nt_kernel;
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  if(!(_flags & flag::win_disable_unlink_emulation))
  {
    // Rename it to something random to emulate immediate unlinking
    auto randomname = utils::random_string(32);
    randomname.append(".deleted");
    BOOST_OUTCOME_TRY(_, relink(std::move(randomname)));
  }
  // No point marking it for deletion if it's already been so
  if(!(_flags & flag::unlink_on_close))
  {
    // Hide the item in Explorer and the command line
    {
      IO_STATUS_BLOCK isb = make_iostatus();
      FILE_BASIC_INFORMATION fbi;
      memset(&fbi, 0, sizeof(fbi));
      fbi.FileAttributes = FILE_ATTRIBUTE_HIDDEN;
      NtSetInformationFile(_v.h, &isb, &fbi, sizeof(fbi), FileBasicInformation);
      if(_flags & flag::overlapped)
        ntwait(_v.h, isb, deadline());
    }
    // Mark the item as delete on close
    IO_STATUS_BLOCK isb = make_iostatus();
    FILE_DISPOSITION_INFORMATION fdi;
    memset(&fdi, 0, sizeof(fdi));
    fdi._DeleteFile = true;
    NtSetInformationFile(_v.h, &isb, &fdi, sizeof(fdi), FileDispositionInformation);
    if(_flags & flag::overlapped)
      ntwait(_v.h, isb, deadline());
    if(STATUS_SUCCESS != isb.Status)
      return make_errored_result_nt<void>(isb.Status);
  }
  return make_valued_result<void>();
}

result<file_handle::extent_type> file_handle::length() const noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  FILE_STANDARD_INFO fsi;
  if(!GetFileInformationByHandleEx(_v.h, FileStandardInfo, &fsi, sizeof(fsi)))
    return make_errored_result<extent_type>(GetLastError());
  return fsi.EndOfFile.QuadPart;
}

result<file_handle::extent_type> file_handle::truncate(file_handle::extent_type newsize) noexcept
{
  BOOST_AFIO_LOG_FUNCTION_CALL(_v.h);
  FILE_END_OF_FILE_INFO feofi;
  feofi.EndOfFile.QuadPart = newsize;
  if(!SetFileInformationByHandle(_v.h, FileEndOfFileInfo, &feofi, sizeof(feofi)))
    return make_errored_result<extent_type>(GetLastError());
  if(are_safety_fsyncs_issued())
  {
    FlushFileBuffers(_v.h);
  }
  return newsize;
}

BOOST_AFIO_V2_NAMESPACE_END
