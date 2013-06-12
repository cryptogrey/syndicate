/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#ifndef _AG_DISK_H
#define _AG_DISK_H_

#include "libgateway.h"

#include <map>

using namespace std;

#define GATEWAY_REQUEST_TYPE_NONE 0
#define GATEWAY_REQUEST_TYPE_LOCAL_FILE 1
#define GATEWAY_REQUEST_TYPE_MANIFEST 2

struct gateway_ctx {
   int request_type;

   // file info 
   char const* file_path;

   // data buffer (manifest or remote block data)
   char* data;
   size_t data_len;
   size_t data_offset;
   off_t num_read;

   // file block info
   uint64_t block_id;

   // input descriptor
   int fd;
};

typedef map<string, struct md_entry*> content_map;


#endif