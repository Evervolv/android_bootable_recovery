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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <cutils/properties.h>

#include <lib/libtar.h>
#include <zlib.h>

#include <fs_mgr.h>
#include "roots.h"

#include "bu.h"

using namespace android;

static int verify_sod() {
  const char* key;
  char value[PROPERTY_VALUE_MAX];
  char sodbuf[PROP_LINE_LEN * 10];
  size_t len;

  len = sizeof(sodbuf);
  if (tar_extract_file_contents(tar, sodbuf, &len) != 0) {
    logmsg("tar_verify_sod: failed to extract file\n");
    return -1;
  }

  char val_hashname[PROPERTY_VALUE_MAX];
  memset(val_hashname, 0, sizeof(val_hashname));
  char val_product[PROPERTY_VALUE_MAX];
  memset(val_product, 0, sizeof(val_product));
  char* p = sodbuf;
  char* q;
  while ((q = strchr(p, '\n')) != NULL) {
    char* key = p;
    *q = '\0';
    logmsg("verify_sod: line=%s\n", p);
    p = q + 1;
    char* val = strchr(key, '=');
    if (val) {
      *val = '\0';
      ++val;
      if (strcmp(key, "hash.name") == 0) {
        strncpy(val_hashname, val, sizeof(val_hashname));
      }
      if (strcmp(key, "ro.product.device") == 0) {
        strncpy(val_product, val, sizeof(val_product));
      }
      if (strncmp(key, "fs.", 3) == 0) {
        char* name = key + 3;
        char* attr = strchr(name, '.');
        if (attr) {
          *attr = '\0';
          ++attr;
          part_add(name);
          struct partspec* part = part_find(name);
          if (!strcmp(attr, "size")) {
            part->size = strtoul(val, NULL, 0);
          }
          if (!strcmp(attr, "used")) {
            part->used = strtoul(val, NULL, 0);
          }
        }
      }
    }
  }

  if (!val_hashname[0]) {
    logmsg("verify_sod: did not find hash.name\n");
    return -1;
  }
  hash_name = strdup(val_hashname);

  if (!val_product[0]) {
    logmsg("verify_sod: did not find ro.product.device\n");
    return -1;
  }
  key = "ro.product.device";
  property_get(key, value, "");
  if (strcmp(val_product, value) != 0) {
    logmsg("verify_sod: product does not match\n");
    return -1;
  }

  return 0;
}

static int verify_eod(size_t actual_hash_datalen, SHA_CTX* actual_sha_ctx,
                      MD5_CTX* actual_md5_ctx) {
  int rc = -1;
  char eodbuf[PROP_LINE_LEN * 10];
  size_t len;

  len = sizeof(eodbuf);
  if (tar_extract_file_contents(tar, eodbuf, &len) != 0) {
    logmsg("verify_eod: failed to extract file\n");
    return -1;
  }

  size_t reported_datalen = 0;
  char reported_hash[HASH_MAX_STRING_LENGTH];
  memset(reported_hash, 0, sizeof(reported_hash));
  char* p = eodbuf;
  char* q;
  while ((q = strchr(p, '\n')) != NULL) {
    char* key = p;
    *q = '\0';
    logmsg("verify_eod: line=%s\n", p);
    p = q + 1;
    char* val = strchr(key, '=');
    if (val) {
      *val = '\0';
      ++val;
      if (strcmp(key, "hash.datalen") == 0) {
        reported_datalen = strtoul(val, NULL, 0);
      }
      if (strcmp(key, "hash.value") == 0) {
        memset(reported_hash, 0, sizeof(reported_hash));
        strncpy(reported_hash, val, sizeof(reported_hash));
      }
    }
  }

  unsigned char digest[HASH_MAX_LENGTH];
  char hexdigest[HASH_MAX_STRING_LENGTH];

  int n;
  if (hash_name != NULL && !strcasecmp(hash_name, "sha1")) {
    SHA1_Final(digest, actual_sha_ctx);
    for (n = 0; n < SHA_DIGEST_LENGTH; ++n) {
      sprintf(hexdigest + 2 * n, "%02x", digest[n]);
    }
  } else {  // default to md5
    MD5_Final(digest, actual_md5_ctx);
    for (n = 0; n < MD5_DIGEST_LENGTH; ++n) {
      sprintf(hexdigest + 2 * n, "%02x", digest[n]);
    }
  }

  logmsg("verify_eod: expected=%d,%s\n", actual_hash_datalen, hexdigest);

  logmsg("verify_eod: reported=%d,%s\n", reported_datalen, reported_hash);

  if ((reported_datalen == actual_hash_datalen) &&
      (memcmp(hexdigest, reported_hash, strlen(hexdigest)) == 0)) {
    rc = 0;
  }

  return rc;
}

int do_restore(int argc, char** argv) {
  int rc = 0;
  ssize_t len;
  const char* compress = "none";
  char buf[512];

  len = recv(adb_ifd, buf, sizeof(buf), MSG_PEEK);
  if (len < 0) {
    logmsg("do_restore: peek failed (%d:%s)\n", rc, strerror(errno));
    return -1;
  }
  if (len < 2) {
    logmsg("do_restore: peek returned %d\n", len);
    return -1;
  }
  if (buf[0] == 0x1f && buf[1] == 0x8b) {
    logmsg("do_restore: is gzip\n");
    compress = "gzip";
  }

  create_tar(adb_ifd, compress, "r");

  size_t save_hash_datalen;
  SHA_CTX save_sha_ctx;
  MD5_CTX save_md5_ctx;

  while (1) {
    save_hash_datalen = hash_datalen;
    memcpy(&save_sha_ctx, &sha_ctx, sizeof(SHA_CTX));
    memcpy(&save_md5_ctx, &md5_ctx, sizeof(MD5_CTX));
    rc = th_read(tar);
    if (rc != 0) {
      if (rc == 1) {  // EOF
        rc = 0;
      }
      break;
    }
    char* pathname = th_get_pathname(tar);
    logmsg("do_restore: extract %s\n", pathname);
    if (!strcmp(pathname, "SOD")) {
      rc = verify_sod();
      logmsg("do_restore: tar_verify_sod returned %d\n", rc);
    } else if (!strcmp(pathname, "EOD")) {
      rc = verify_eod(save_hash_datalen, &save_sha_ctx, &save_md5_ctx);
      logmsg("do_restore: tar_verify_eod returned %d\n", rc);
    } else {
      char mnt[PATH_MAX];
      snprintf(mnt, sizeof(mnt), "/%s", pathname);
      Volume* vol = volume_for_mount_point(mnt);
      if (vol != NULL && vol->fs_type != NULL) {
        partspec* curpart = part_find(pathname);
        part_set(curpart);
        rc = tar_extract_file(tar, vol->blk_device);
      } else {
        logmsg("do_restore: cannot find volume for %s\n", mnt);
      }
    }
    free(pathname);
    if (rc != 0) {
      logmsg("do_restore: extract failed, rc=%d\n", rc);
      break;
    }
  }

  tar_close(tar);
  logmsg("do_restore: rc=%d\n", rc);

  free(hash_name);
  hash_name = NULL;

  return rc;
}
