/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#ifndef _URL_H_
#define _URL_H_

#include "fs_entry.h"

char* fs_entry_block_url( struct fs_core* core, uint64_t volume_id, char const* base_url, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version, bool local, bool staging );
char* fs_entry_local_block_url( struct fs_core* core, uint64_t file_id, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_local_staging_block_url( struct fs_core* core, uint64_t file_id, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_public_block_url( struct fs_core* core, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_public_staging_block_url( struct fs_core* core, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_remote_block_url( struct fs_core* core, uint64_t gateway_id, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_replica_block_url( struct fs_core* core, char* RG_url, uint64_t volume_id, uint64_t file_id, int64_t file_version, uint64_t block_id, int64_t block_version );
char* fs_entry_AG_block_url( struct fs_core* core, uint64_t ag_id, char const* fs_path, int64_t version, uint64_t block_id, int64_t block_version );
char* fs_entry_block_url_path( struct fs_core* core, char const* fs_path, int64_t version, uint64_t block_id, int64_t block_version );

char* fs_entry_file_url( struct fs_core* core, uint64_t volume_id, char const* base_url, char const* fs_path, int64_t file_version, bool local, bool staging );
char* fs_entry_local_file_url( struct fs_core* core, uint64_t file_id, int64_t file_version );
char* fs_entry_local_staging_file_url( struct fs_core* core, uint64_t file_id, int64_t file_version );
char* fs_entry_public_file_url( struct fs_core* core, char const* fs_path, int64_t file_version );
char* fs_entry_public_staging_file_url( struct fs_core* core, char const* fs_path, int64_t file_version );

char* fs_entry_manifest_url( struct fs_core* core, char const* gateway_base_url, uint64_t volume_id, char* fs_path, int64_t version, struct timespec* ts );
char* fs_entry_public_manifest_url( struct fs_core* core, char const* fs_path, int64_t version, struct timespec* ts );
char* fs_entry_remote_manifest_url( struct fs_core* core, uint64_t UG_id, char const* fs_path, int64_t version, struct timespec* ts );
char* fs_entry_replica_manifest_url( struct fs_core* core, char const* RG_url, uint64_t volume_id, uint64_t file_id, int64_t version, struct timespec* ts );
char* fs_entry_manifest_url_path( struct fs_core* core, char const* fs_path, int64_t version, struct timespec* ts );

#endif 
