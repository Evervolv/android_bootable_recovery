/*
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

#include <utils/String8.h>

#include <lib/libtar.h>
#include <zlib.h>

extern "C" {
#include <openssl/md5.h>
#include <openssl/sha.h>
#ifndef MD5_DIGEST_STRING_LENGTH
#define MD5_DIGEST_STRING_LENGTH (MD5_DIGEST_LENGTH * 2 + 1)
#endif
#ifndef SHA_DIGEST_STRING_LENGTH
#define SHA_DIGEST_STRING_LENGTH (SHA_DIGEST_LENGTH * 2 + 1)
#endif
}

#define HASH_MAX_LENGTH SHA_DIGEST_LENGTH
#define HASH_MAX_STRING_LENGTH SHA_DIGEST_STRING_LENGTH

#define PROP_LINE_LEN (PROPERTY_KEY_MAX + 1 + PROPERTY_VALUE_MAX + 1 + 1)

extern int adb_ifd;
extern int adb_ofd;
extern TAR* tar;
extern gzFile gzf;

extern char* hash_name;
extern size_t hash_datalen;
extern SHA_CTX sha_ctx;
extern MD5_CTX md5_ctx;

struct partspec {
  char* name;
  char* path;
  Volume* vol;
  uint64_t size;
  uint64_t used;
  uint64_t off;
};
#define MAX_PART 8

extern void logmsg(const char* fmt, ...);

extern int part_add(const char* name);
extern partspec* part_get(int i);
extern partspec* part_find(const char* name);
extern void part_set(partspec* part);

extern int update_progress(uint64_t off);

extern int create_tar(int fd, const char* compress, const char* mode);

extern int do_backup(int argc, char** argv);
extern int do_restore(int argc, char** argv);
