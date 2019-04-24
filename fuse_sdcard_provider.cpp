/*
 * Copyright (C) 2014 The Android Open Source Project
 * Copyright (C) 2019 The LineageOS Project
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

#include "fuse_sdcard_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>

#include <android-base/file.h>

#include "fuse_sideload.h"

struct file_data {
  int fd;  // the underlying sdcard file

  uint64_t file_size;
  uint32_t block_size;
};

static int read_block_file(const file_data& fd, uint32_t block, uint8_t* buffer,
                           uint32_t fetch_size) {
  off64_t offset = static_cast<off64_t>(block) * fd.block_size;
  if (TEMP_FAILURE_RETRY(lseek64(fd.fd, offset, SEEK_SET)) == -1) {
    fprintf(stderr, "seek on sdcard failed: %s\n", strerror(errno));
    return -EIO;
  }

  if (!android::base::ReadFully(fd.fd, buffer, fetch_size)) {
    fprintf(stderr, "read on sdcard failed: %s\n", strerror(errno));
    return -EIO;
  }

  return 0;
}

struct token {
  pid_t pid;
  const char* path;
  int result;
};

static void* run_sdcard_fuse(void* cookie) {
  token* t = reinterpret_cast<token*>(cookie);

  struct stat sb;
  if (stat(t->path, &sb) < 0) {
    fprintf(stderr, "failed to stat %s: %s\n", t->path, strerror(errno));
    t->result = -1;
    return nullptr;
  }

  struct file_data fd;
  struct provider_vtab vtab;

  fd.fd = open(t->path, O_RDONLY);
  if (fd.fd < 0) {
    fprintf(stderr, "failed to open %s: %s\n", t->path, strerror(errno));
    t->result = -1;
    return nullptr;
  }
  fd.file_size = sb.st_size;
  fd.block_size = 65536;

  vtab.read_block = std::bind(&read_block_file, fd, std::placeholders::_1, std::placeholders::_2,
                              std::placeholders::_3);
  vtab.close = [&fd]() { close(fd.fd); };

  t->result = run_fuse_sideload(vtab, fd.file_size, fd.block_size);
  return nullptr;
}

// How long (in seconds) we wait for the fuse-provided package file to
// appear, before timing out.
#define SDCARD_INSTALL_TIMEOUT 10

void* start_sdcard_fuse(const char* path) {
  token* t = new token;

  t->path = path;
  if ((t->pid = fork()) < 0) {
    free(t);
    return nullptr;
  }
  if (t->pid == 0) {
    run_sdcard_fuse(t);
    _exit(0);
  }

  time_t start_time = time(nullptr);
  time_t now = start_time;

  while (now - start_time < SDCARD_INSTALL_TIMEOUT) {
    struct stat st;
    if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &st) == 0) {
      break;
    }
    if (errno != ENOENT && errno != ENOTCONN) {
      free(t);
      t = nullptr;
      break;
    }
    sleep(1);
    now = time(nullptr);
  }

  return t;
}

void finish_sdcard_fuse(void* cookie) {
  if (cookie == NULL) return;
  token* t = reinterpret_cast<token*>(cookie);

  kill(t->pid, SIGTERM);
  int status;
  waitpid(t->pid, &status, 0);

  delete t;
}
