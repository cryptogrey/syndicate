/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/

#include "mkdir.h"

// low-level mkdir
int fs_entry_mkdir_lowlevel( struct fs_core* core, char const* path, struct fs_entry* parent, char const* path_basename, mode_t mode, uid_t user, gid_t vol, int64_t mtime_sec, int32_t mtime_nsec ) {
   
   // resolve the child
   struct fs_entry* child = fs_entry_set_find_name( parent->children, path_basename );
   int err = 0;
   if( child == NULL ) {

      // success.  Make local storage
      err = fs_entry_create_local_directory( core, path );
      if( err != 0 ) {
         return err;
      }
      else {
         // create an fs_entry and attach it 
         child = (struct fs_entry*)calloc( sizeof(struct fs_entry), 1 );

         // generate a URL
         char* url = fs_entry_public_dir_url( core, path );
         
         fs_entry_init_dir( core, child, path_basename, url, fs_entry_next_file_version(), user, vol, mode, 4096, mtime_sec, mtime_nsec );
         free( url );

         fs_core_fs_rlock( core );

         child->link_count++;

         // add . and ..
         fs_entry_set_insert( child->children, ".", child );
         fs_entry_set_insert( child->children, "..", parent );

         fs_entry_set_insert( parent->children, path_basename, child );

         fs_core_fs_unlock( core );
      }
   }
   else {
      // already exists
      err = -EEXIST;
   }

   return err;
}


// create a directory
int fs_entry_mkdir( struct fs_core* core, char const* path, mode_t mode, uid_t user, gid_t vol ) {
   int err = 0;

   // resolve the parent of this child (and write-lock it)
   char* fpath = strdup( path );
   md_sanitize_path( fpath );
   
   // revalidate this path
   int rc = fs_entry_revalidate_path( core, fpath );
   if( rc != 0 ) {
      // consistency cannot be guaranteed
      errorf("fs_entry_revalidate_path(%s) rc = %d\n", fpath, rc );
      free( fpath );
      return -EREMOTEIO;
   }
   
   char* path_dirname = md_dirname( fpath, NULL );
   char* path_basename = md_basename( fpath, NULL );

   md_sanitize_path( path_dirname );

   free( fpath );

   struct fs_entry* parent = fs_entry_resolve_path( core, path_dirname, user, vol, true, &err );
   

   if( parent == NULL || err ) {
      // parent not found
      free( path_basename );
      free( path_dirname );
      // err is set appropriately
      return err;
   }
   if( parent->ftype != FTYPE_DIR ) {
      // parent is not a directory
      fs_entry_unlock( parent );
      free( path_basename );
      free( path_dirname );
      return -ENOTDIR;
   }
   if( !IS_WRITEABLE(parent->mode, parent->owner, parent->volume, user, vol) ) {
      // parent is not writeable
      errorf( "%s is not writable by %d (%o, %d, %d, %d, %d)\n", path_dirname, user, parent->mode, parent->owner, parent->volume, user, vol );
      fs_entry_unlock( parent );
      free( path_basename );
      free( path_dirname );
      return -EACCES;
   }

   struct timespec ts;
   clock_gettime( CLOCK_REALTIME, &ts );

   err = fs_entry_mkdir_lowlevel( core, path, parent, path_basename, mode, user, vol, ts.tv_sec, ts.tv_nsec );

   if( err != 0 ) {
      errorf( "fs_entry_mkdir_lowlevel(%s) rc = %d\n", path, err );
      fs_entry_unlock( parent );
      free( path_basename );
      free( path_dirname );
      return -1;
   }
   
   parent->mtime_sec = ts.tv_sec;
   parent->mtime_nsec = ts.tv_nsec;

   struct fs_entry* child = fs_entry_set_find_name( parent->children, path_basename );

   if( err == 0 ) {
      fs_entry_rlock( child );

      // create this directory in the MS
      struct md_entry data;
      fs_entry_to_md_entry( core, path, child, &data );

      err = ms_client_mkdir( core->ms, &data );

      if( err != 0 ) {
         errorf("ms_client_create(%s) rc = %d\n", data.path, err );
         err = -EREMOTEIO;

         // undo this creation
         int rc = fs_entry_remove_local_directory( core, data.path );
         if( rc != 0 ) {
            errorf("fs_entry_remove_local_directory(%s) rc = %d\n", data.path, rc );
         }
         
         rc = fs_entry_detach_lowlevel( core, parent, child, true );
         if( rc != 0 ) {
            errorf("fs_entry_detach_lowlevel(%s) rc = %d\n", data.path, rc );
         }
      }
      else {
         fs_core_wlock( core );
         core->num_files++;
         fs_core_unlock( core );
      }

      fs_entry_unlock( child );
      
      md_entry_free( &data );
   }

   fs_entry_unlock( parent );
   free( path_basename );
   free( path_dirname );
   return err;
}