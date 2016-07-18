/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "crash_collector"

#include "coredump_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <log/logger.h>

// From external/google-breakpad.
#include "common/linux/elf_core_dump.h"

namespace {

const size_t kMaxCoredumpSize = 256 * 1024 * 1024;

int64_t GetFreeDiskSpace(const std::string& path) {
  struct statvfs stats;
  if (TEMP_FAILURE_RETRY(statvfs(path.c_str(), &stats)) != 0) {
    ALOGE("statvfs() failed. errno = %d", errno);
    return -1;
  }
  return static_cast<int64_t>(stats.f_bavail) * stats.f_frsize;
}

bool Seek(int fd, off_t offset) {
  return lseek(fd, offset, SEEK_SET) == offset;
}

template<typename T>
T GetValueFromNote(const google_breakpad::ElfCoreDump::Note& note,
                   size_t offset,
                   T default_value) {
  const T* p = note.GetDescription().GetData<T>(offset);
  return p ? *p : default_value;
}

}  // namespace

class CoredumpWriter::FdReader {
 public:
  explicit FdReader(int fd) : fd_(fd), bytes_read_(0) {}

  // Reads the given number of bytes.
  bool Read(void* buf, size_t num_bytes) {
    if (!android::base::ReadFully(fd_, buf, num_bytes))
      return false;
    bytes_read_ += num_bytes;
    return true;
  }

  // Reads the given number of bytes and writes it to fd_dest.
  bool CopyTo(int fd_dest, size_t num_bytes) {
    const size_t kBufSize = 32768;
    char buf[kBufSize];
    while (num_bytes > 0) {
      int rv = TEMP_FAILURE_RETRY(
          read(fd_, buf, std::min(kBufSize, num_bytes)));
      if (rv == 0)
        break;
      if (rv == -1)
        return false;
      if (fd_dest != -1 && !android::base::WriteFully(fd_dest, buf, rv))
        return false;
      num_bytes -= rv;
      bytes_read_ += rv;
    }
    return num_bytes == 0;
  }

  // Reads data and discards it to get to the specified position.
  bool Seek(size_t offset) {
    if (offset < bytes_read_)  // Cannot move backward.
      return false;
    return CopyTo(-1, offset - bytes_read_);
  }

 private:
  int fd_;
  size_t bytes_read_;

  DISALLOW_COPY_AND_ASSIGN(FdReader);
};

CoredumpWriter::CoredumpWriter(int fd_src,
                               const std::string& coredump_filename,
                               const std::string& proc_files_dir)
    : fd_src_(fd_src),
      coredump_filename_(coredump_filename),
      proc_files_dir_(proc_files_dir) {
}

CoredumpWriter::~CoredumpWriter() {
}

ssize_t CoredumpWriter::WriteCoredump() {
  android::base::unique_fd fd_dest(
      TEMP_FAILURE_RETRY(open(coredump_filename_.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL,
                              S_IRUSR | S_IWUSR)));
  if (fd_dest == -1) {
    ALOGE("Failed to open: %s, errno = %d", coredump_filename_.c_str(), errno);
    return -1;
  }
  ssize_t result = WriteCoredumpToFD(fd_dest);
  fd_dest.reset(-1);
  if (result == -1)
    unlink(coredump_filename_.c_str());
  return result;
}

ssize_t CoredumpWriter::WriteCoredumpToFD(int fd_dest) {
  // Input coredump is generated by kernel's fs/binfmt_elf.c and formatted like:
  //
  //   ELF Header
  //   Program Header 1
  //   Program Header 2
  //   ...
  //   Program Header n
  //   Segment 1 (This segment's type should be PT_NOTE)
  //   Segment 2
  //   ...
  //   Segment n

  // First, read ELF Header, all program headers, and the first segment whose
  // type is PT_NOTE.
  FdReader reader(fd_src_);
  Ehdr elf_header;
  std::vector<Phdr> program_headers;
  std::vector<char> note_buf;
  if (!ReadUntilNote(&reader, &elf_header, &program_headers, &note_buf)) {
    return -1;
  }
  // Get a set of address ranges occupied by mapped files from NOTE.
  FileMappings file_mappings;
  if (!GetFileMappings(note_buf, &file_mappings)) {
    return -1;
  }
  // Filter out segments backed by mapped files as they are useless when
  // generating minidump.
  std::vector<Phdr> program_headers_filtered;
  FilterSegments(program_headers, file_mappings, &program_headers_filtered);

  // Calculate the coredump size limit.
  const int64_t free_disk_space = GetFreeDiskSpace(coredump_filename_);
  if (free_disk_space < 0) {
    return -1;
  }
  coredump_size_limit_ = std::min(static_cast<size_t>(free_disk_space / 20),
                                  kMaxCoredumpSize);

  // Calculate the output file size.
  expected_coredump_size_ = program_headers_filtered.back().p_offset +
      program_headers_filtered.back().p_filesz;
  if (expected_coredump_size_ > coredump_size_limit_) {
    ALOGE("Coredump too large: %zu", expected_coredump_size_);
    return -1;
  }

  // Write proc files.
  if (!WriteAuxv(note_buf, proc_files_dir_ + "/auxv") ||
      !WriteMaps(program_headers, file_mappings, proc_files_dir_ + "/maps")) {
    return -1;
  }

  // Write ELF header.
  if (!android::base::WriteFully(fd_dest, &elf_header, sizeof(elf_header))) {
    ALOGE("Failed to write ELF header.");
    return -1;
  }
  // Write program headers.
  for (size_t i = 0; i < program_headers_filtered.size(); ++i) {
    const Phdr& program_header = program_headers_filtered[i];
    const size_t offset = sizeof(elf_header) + i * elf_header.e_phentsize;
    if (!Seek(fd_dest, offset) ||
        !android::base::WriteFully(fd_dest, &program_header,
                                   sizeof(program_header))) {
      ALOGE("Failed to write program header: i = %zu", i);
      return -1;
    }
  }
  // Write NOTE segment.
  if (!Seek(fd_dest, program_headers_filtered[0].p_offset) ||
      !android::base::WriteFully(fd_dest, note_buf.data(), note_buf.size())) {
    ALOGE("Failed to write NOTE.");
    return -1;
  }
  // Read all remaining segments and write some of them.
  for (size_t i = 1; i < program_headers_filtered.size(); ++i) {
    const Phdr& program_header = program_headers_filtered[i];
    if (program_header.p_filesz > 0) {
      const Phdr& program_header_original = program_headers[i];
      if (!reader.Seek(program_header_original.p_offset)) {
        ALOGE("Failed to seek segment: i = %zu", i);
        return -1;
      }
      if (!Seek(fd_dest, program_header.p_offset) ||
          !reader.CopyTo(fd_dest, program_header.p_filesz)) {
        ALOGE("Failed to write segment: i = %zu", i);
        return -1;
      }
    }
  }
  return expected_coredump_size_;
}

bool CoredumpWriter::ReadUntilNote(FdReader* reader,
                                   Ehdr* elf_header,
                                   std::vector<Phdr>* program_headers,
                                   std::vector<char>* note_buf) {
  // Read ELF header.
  if (!reader->Read(elf_header, sizeof(*elf_header)) ||
      memcmp(elf_header->e_ident, ELFMAG, SELFMAG) != 0 ||
      elf_header->e_ident[EI_CLASS] != google_breakpad::ElfCoreDump::kClass ||
      elf_header->e_version != EV_CURRENT ||
      elf_header->e_type != ET_CORE ||
      elf_header->e_ehsize != sizeof(Ehdr) ||
      elf_header->e_phentsize != sizeof(Phdr)) {
    ALOGE("Failed to read ELF header.");
    return false;
  }

  // Read program headers;
  program_headers->resize(elf_header->e_phnum);
  if (!reader->Seek(elf_header->e_phoff) ||
      !reader->Read(program_headers->data(),
                    sizeof(Phdr) * program_headers->size())) {
    ALOGE("Failed to read program headers.");
    return false;
  }

  // The first segment should be NOTE.
  if (program_headers->size() < 1 ||
      (*program_headers)[0].p_type != PT_NOTE) {
    ALOGE("Failed to locate NOTE.");
    return false;
  }
  const Phdr& note_program_header = (*program_headers)[0];

  // Read NOTE segment.
  note_buf->resize(note_program_header.p_filesz);
  if (!reader->Seek(note_program_header.p_offset) ||
      !reader->Read(note_buf->data(), note_buf->size())) {
    ALOGE("Failed to read NOTE.");
    return false;
  }
  return true;
}

bool CoredumpWriter::GetFileMappings(const std::vector<char>& note_buf,
                                     FileMappings* file_mappings) {
  // Locate FILE note.
  google_breakpad::ElfCoreDump::Note note(
      google_breakpad::MemoryRange(note_buf.data(), note_buf.size()));
  while (note.IsValid() && note.GetType() != NT_FILE) {
    note = note.GetNextNote();
  }
  if (!note.IsValid()) {
    ALOGE("Failed to locate NT_FILE.");
    return false;
  }

  // NT_FILE note format: (see kernel's fs/binfmt_elf.c for details)
  //   Number of mapped files
  //   Page size
  //   Start address of file 1
  //   End address of file 1
  //   Offset of file 1
  //   Start address of file 2
  //   ...
  //   Offset of file n
  //   File name 1 (null-terminated)
  //   File name 2
  //   ...
  //   File name n
  const long kInvalidValue = -1;
  const long file_count = GetValueFromNote<long>(note, 0, kInvalidValue);
  const long page_size = GetValueFromNote<long>(note, sizeof(long),
                                                kInvalidValue);
  if (file_count == kInvalidValue || page_size == kInvalidValue) {
    ALOGE("Invalid FILE note.");
    return false;
  }
  // Read contents of FILE note.
  size_t filename_pos = sizeof(long) * (2 + 3 * file_count);
  for (long i = 0; i < file_count; ++i) {
    const long start = GetValueFromNote<long>(
        note, sizeof(long) * (2 + 3 * i), kInvalidValue);
    const long end = GetValueFromNote<long>(
        note, sizeof(long) * (2 + 3 * i + 1), kInvalidValue);
    const long offset = GetValueFromNote<long>(
        note, sizeof(long) * (2 + 3 * i + 2), kInvalidValue);
    if (start == kInvalidValue || end == kInvalidValue ||
        offset == kInvalidValue) {
      ALOGE("Invalid FILE Note.");
      return false;
    }
    // Add a new mapping.
    FileInfo& info = (*file_mappings)[std::make_pair(start, end)];
    info.offset = offset * page_size;
    // Read file name.
    while (true) {
      const char c = GetValueFromNote<char>(note, filename_pos++, 0);
      if (!c)
        break;
      info.path.push_back(c);
    }
  }
  return true;
}

void CoredumpWriter::FilterSegments(
    const std::vector<Phdr>& program_headers,
    const FileMappings& file_mappings,
    std::vector<Phdr>* program_headers_filtered) {
  program_headers_filtered->resize(program_headers.size());

  // The first segment is NOTE. Use the original data unchanged.
  (*program_headers_filtered)[0] = program_headers[0];

  for (size_t i = 1; i < program_headers.size(); ++i) {
    Phdr& out = (*program_headers_filtered)[i];
    out = program_headers[i];

    // If the type is PT_LOAD and the range is found in the set, it means the
    // segment is backed by a file.  So it can be excluded as it doesn't cotnain
    // stack data useful to generate minidump.
    const FileRange range(out.p_vaddr, out.p_vaddr + out.p_memsz);
    if (out.p_type == PT_LOAD && file_mappings.count(range)) {
      out.p_filesz = 0;
    }
    // Calculate offset.
    const Phdr& prev_program_header = (*program_headers_filtered)[i - 1];
    out.p_offset = prev_program_header.p_offset + prev_program_header.p_filesz;
    // Offset alignment.
    if (out.p_align != 0 && out.p_offset % out.p_align != 0) {
      out.p_offset += out.p_align - out.p_offset % out.p_align;
    }
  }
}

bool CoredumpWriter::WriteAuxv(const std::vector<char>& note_buf,
                               const std::string& output_path) {
  // Locate AUXV note.
  google_breakpad::ElfCoreDump::Note note(
      google_breakpad::MemoryRange(note_buf.data(), note_buf.size()));
  while (note.IsValid() && note.GetType() != NT_AUXV) {
    note = note.GetNextNote();
  }
  if (!note.IsValid()) {
    ALOGE("Failed to locate NT_AUXV.");
    return false;
  }

  android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(
      output_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_EXCL,
      S_IRUSR | S_IWUSR)));
  if (fd == -1) {
    ALOGE("Failed to open %s", output_path.c_str());
    return false;
  }
  // The contents of NT_AUXV is in the same format as that of /proc/[pid]/auxv.
  return android::base::WriteFully(
      fd, note.GetDescription().data(), note.GetDescription().length());
}

bool CoredumpWriter::WriteMaps(const std::vector<Phdr>& program_headers,
                               const FileMappings& file_mappings,
                               const std::string& output_path) {
  android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(
      output_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_EXCL,
      S_IRUSR | S_IWUSR)));
  if (fd == -1) {
    ALOGE("Failed to open %s", output_path.c_str());
    return false;
  }
  for (const auto& program_header : program_headers) {
    if (program_header.p_type != PT_LOAD)
      continue;
    const FileRange range(program_header.p_vaddr,
                          program_header.p_vaddr + program_header.p_memsz);
    // If a mapping is found for the range, the range is mapped to a file.
    const auto it = file_mappings.find(range);
    const long offset = it != file_mappings.end() ? it->second.offset : 0;
    const std::string path = it != file_mappings.end() ? it->second.path : "";

    const int kBufSize = 1024;
    char buf[kBufSize];
    const int len = snprintf(
        buf, kBufSize, "%08lx-%08lx %c%c%c%c %08lx %02x:%02x %d %s\n",
        range.first, range.second,
        program_header.p_flags & PF_R ? 'r' : '-',
        program_header.p_flags & PF_W ? 'w' : '-',
        program_header.p_flags & PF_X ? 'x' : '-',
        'p',  // Fake value: We can't know if the mapping is shared or private.
        offset,
        0,  // Fake device (major) value.
        0,  // Fake device (minor) value.
        0,  // Fake inode value.
        path.c_str());
    if (len < 0 || len > kBufSize ||
        !android::base::WriteFully(fd, buf, len)) {
      return false;
    }
  }
  return true;
}
