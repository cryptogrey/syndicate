/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#include "replication.h"

static ReplicaUploader* rutp;

// construct an upload state machine
ReplicaUpload::ReplicaUpload( struct md_syndicate_conf* conf ) {
   this->fh = NULL;
   this->verify_peer = conf->verify_peer;
   this->path = NULL;
   this->form_data = NULL;
   this->ref_count = 0;
   this->status = CALLOC_LIST( int, this->ref_count );
   this->file = NULL;
   pthread_mutex_init( &this->lock, NULL );
}

// free an upload state machine
ReplicaUpload::~ReplicaUpload() {
   if( this->file ) {
      fclose( this->file );
   }
   if( this->path ) {
      free( this->path );
   }
   if( this->form_data ) {
      curl_formfree( this->form_data );
   }
   if( this->data ) {
      free( this->data );
   }
   if( this->status ) {
      free( this->status );
   }
   pthread_mutex_destroy( &this->lock );
}

/*
// debugging purposes
static int replica_trace( CURL* curl_h, curl_infotype type, char* data, size_t size, void* user ) {
   if( type != CURLINFO_TEXT ) {
      FILE* f = (FILE*)user;
      fwrite( data, 1, size, f );
      fflush( f );
   }
   return 0;
}
*/


// curl read function for uploading
size_t replica_curl_read( void* ptr, size_t size, size_t nmemb, void* userdata ) {
   FILE* file = (FILE*)userdata;
   if( file ) {
      size_t sz = fread( ptr, size, nmemb, file );
      return sz * size;
   }
   else {
      errorf("%s", " no userdata\n");
      return CURL_READFUNC_ABORT;
   }
}


// replicate a manifest
int ReplicaUpload::setup_manifest( struct fs_file_handle* fh, char* manifest_data, size_t manifest_data_len, char const* fs_path, int64_t file_version, int64_t mtime_sec, int32_t mtime_nsec, bool verify_peer ) {
   // build an update
   ms::ms_gateway_blockinfo replica_info;
   replica_info.set_fs_path( string(fs_path) );
   replica_info.set_file_version( file_version );
   replica_info.set_block_id( 0 );
   replica_info.set_block_version( 0 );
   replica_info.set_blocking_factor( 0 );
   replica_info.set_file_mtime_sec( mtime_sec );
   replica_info.set_file_mtime_nsec( mtime_nsec );

   string replica_info_str;
   bool src = replica_info.SerializeToString( &replica_info_str );
   if( !src ) {
      errorf("%s", " failed to serialize\n");
      return -EINVAL;
   }

   char* fs_fullpath = fs_entry_manifest_path( "", fs_path, file_version, mtime_sec, mtime_nsec );
   
   this->path = fs_fullpath;
   struct curl_httppost* last = NULL;
   this->form_data = NULL;

   curl_formadd( &this->form_data, &last, CURLFORM_COPYNAME, "metadata",
                                          CURLFORM_CONTENTSLENGTH, replica_info_str.size(),
                                          CURLFORM_COPYCONTENTS, replica_info_str.c_str(),
                                          CURLFORM_END );

   curl_formadd( &this->form_data, &last, CURLFORM_COPYNAME, "data",
                                          CURLFORM_PTRCONTENTS, manifest_data,
                                          CURLFORM_CONTENTSLENGTH, manifest_data_len,
                                          CURLFORM_END );

   this->verify_peer = verify_peer;
   this->data = manifest_data;
   this->fh = fh;
   return 0;
   
}
                                   
// set up our handle
int ReplicaUpload::setup_block( struct fs_file_handle* fh, char const* data_root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version, int64_t mtime_sec, int32_t mtime_nsec, uint64_t blocking_factor, bool verify_peer ) {
   // attempt to open the file
   char* local_path = fs_entry_local_block_path( data_root, fs_path, file_version, block_id, block_version );

   if( this->file == NULL ) {
      // stream data from file
      this->file = fopen( local_path, "r" );
      if( this->file == NULL ) {
         int errsv = -errno;
         errorf( "fopen(%s) errno = %d\n", local_path, errsv );
         free( local_path );
         return errsv;
      }

      // stat this to get its size
      struct stat sb;
      int rc = fstat( fileno(this->file), &sb );
      if( rc != 0 ) {
         int errsv = -errno;
         errorf( "fstat errno = %d\n", errsv );
         free( local_path );
         fclose( this->file );
         this->file = NULL;
         return errsv;
      }

      // build an update
      ms::ms_gateway_blockinfo replica_info;
      replica_info.set_fs_path( string(fs_path) );
      replica_info.set_file_version( file_version );
      replica_info.set_block_id( block_id );
      replica_info.set_block_version( block_version );
      replica_info.set_blocking_factor( blocking_factor );
      replica_info.set_file_mtime_sec( mtime_sec );
      replica_info.set_file_mtime_nsec( mtime_nsec );

      string replica_info_str;
      bool src = replica_info.SerializeToString( &replica_info_str );
      if( !src ) {
         errorf("%s", " failed to serialize\n");
         free( local_path );
         fclose( this->file );
         this->file = NULL;
         return -EINVAL;
      }

      char* fs_fullpath = fs_entry_local_block_path( "", fs_path, file_version, block_id, block_version );
      this->path = fs_fullpath;
      struct curl_httppost* last = NULL;
      this->form_data = NULL;

      curl_formadd( &this->form_data, &last, CURLFORM_COPYNAME, "metadata",
                                             CURLFORM_CONTENTSLENGTH, replica_info_str.size(),
                                             CURLFORM_COPYCONTENTS, replica_info_str.c_str(),
                                             CURLFORM_END );
      
      curl_formadd( &this->form_data, &last, CURLFORM_COPYNAME, "data",
                                             CURLFORM_STREAM, this->file,
                                             CURLFORM_FILENAME, this->path,
                                             CURLFORM_CONTENTSLENGTH, (long)sb.st_size,
                                             CURLFORM_END );

      this->verify_peer = verify_peer;
      this->fh = fh;
      free( local_path );
      return 0;
   }
   else {
      free( local_path );
      return -EINVAL;
   }
}


// make an uploader
ReplicaUploader::ReplicaUploader( struct md_syndicate_conf* conf ) :
   CURLTransfer( 1 ) {
      
   this->conf = conf;
   this->num_replica_servers = 0;
   this->replica_servers = NULL;
   this->headers = NULL;
   pthread_mutex_init( &this->download_lock, NULL );
   pthread_mutex_init( &this->running_lock, NULL );
   
   if( conf->replica_urls != NULL ) {
      
      SIZE_LIST( &this->num_replica_servers, conf->replica_urls );

      this->replica_servers = CALLOC_LIST( struct replica_server_channel, num_replica_servers );
      this->headers = CALLOC_LIST( struct curl_slist*, num_replica_servers );
      
      for( int i = 0; conf->replica_urls[i] != NULL; i++ ) {

         this->replica_servers[i].curl_h = curl_easy_init();
         this->replica_servers[i].id = i;
         this->replica_servers[i].pending = new upload_list();
         this->replica_servers[i].url = strdup( conf->replica_urls[i] );
         
         pthread_mutex_init( &this->replica_servers[i].pending_lock, NULL );
         
         md_init_curl_handle( this->replica_servers[i].curl_h, conf->replica_urls[i], conf->metadata_connect_timeout );
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_POST, 1L );
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_SSL_VERIFYPEER, (conf->verify_peer ? 1L : 0L) );
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_READFUNCTION, replica_curl_read );

         // disable Expect: 100-continue
         this->headers[i] = curl_slist_append(this->headers[i], "Expect");
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_HTTPHEADER, this->headers[i] );
         
         /*
         FILE* f = fopen( "/tmp/replica.trace", "w" );
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_DEBUGFUNCTION, replica_trace );
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_DEBUGDATA, f );
         curl_easy_setopt( this->replica_servers[i].curl_h, CURLOPT_VERBOSE, 1L );
         */
         
         this->add_curl_easy_handle( 0, this->replica_servers[i].curl_h );
      }
   }
}

// free memory 
ReplicaUploader::~ReplicaUploader() {
   this->running = false;

   if( !this->stopped ) {
      dbprintf("%s", "Waiting for threads to die...\n");
      while( !this->stopped ) {
         sleep(1);
      }
   }
   
   pthread_join( this->thread, NULL );

   pthread_mutex_lock( &this->download_lock );
   
   for( int i = 0; i < this->num_replica_servers; i++ ) {
      this->remove_curl_easy_handle( 0, this->replica_servers[i].curl_h );
      curl_easy_cleanup( this->replica_servers[i].curl_h );

      for( unsigned int j = 0; j < this->replica_servers[i].pending->size(); j++ ) {
         int refs = (*this->replica_servers[i].pending)[j]->unref();
         if( refs == 0 ) {
            delete (*this->replica_servers[i].pending)[j];
            (*this->replica_servers[i].pending)[j] = NULL;
         }
      }
      
      delete this->replica_servers[i].pending;
      pthread_mutex_destroy( &this->replica_servers[i].pending_lock );
   }

   if( this->replica_servers )
      free( this->replica_servers );

   for( int i = 0; i < this->num_replica_servers; i++ ) {
      curl_slist_free_all( this->headers[i] );
   }
   free( this->headers );

   pthread_mutex_unlock( &this->download_lock );
   pthread_mutex_destroy( &this->download_lock );
   pthread_mutex_destroy( &this->running_lock );
}

// get the next ready handle
// NOTE: must have locks first!
struct replica_server_channel* ReplicaUploader::next_ready_server( int* err ) {

   CURLMsg* msg = NULL;
   *err = 0;
   struct replica_server_channel* rsc = NULL;

   do {
      msg = this->get_next_curl_msg( 0 );
      if( msg ) {
         if( msg->msg == CURLMSG_DONE ) {
            for( int i = 0; i < this->num_replica_servers; i++ ) {

               if( msg->easy_handle == this->replica_servers[i].curl_h ) {
                  rsc = &this->replica_servers[i];
                  break;
               }
            }
         }
      }
   } while( msg != NULL && rsc == NULL );

   if( rsc != NULL ) {
      // ensure that the download finished successfully
      if( msg->data.result != 0 ) {
         *err = msg->data.result;
      }
   }
   return rsc;
}


int ReplicaUploader::unref_replica_upload( ReplicaUpload* ru ) {
   int rc = ru->unref();

   if( rc == 0 ) {
      delete ru;

      pthread_mutex_lock( &this->running_lock );
      this->running_refs.erase( ru->fh );
      pthread_mutex_unlock( &this->running_lock );
   }
   else {
      pthread_mutex_lock( &this->running_lock );
      this->running_refs[ru->fh] = rc;
      pthread_mutex_unlock( &this->running_lock );
   }

   return rc;
}

int ReplicaUploader::ref_replica_upload( ReplicaUpload* ru ) {
   int rc = ru->ref();
   pthread_mutex_lock( &this->running_lock );
   this->running_refs[ru->fh] = rc;
   pthread_mutex_unlock( &this->running_lock );
   return rc;
}

// start replicating on a channel
void ReplicaUploader::enqueue_replica( struct replica_server_channel* rsc, ReplicaUpload* ru ) {
   this->ref_replica_upload( ru );
   
   pthread_mutex_lock( &rsc->pending_lock );
   rsc->pending->push_back( ru );
   pthread_mutex_unlock( &rsc->pending_lock );
}

// finish a replica
void ReplicaUploader::finish_replica( struct replica_server_channel* rsc, int status ) {
   pthread_mutex_lock( &this->download_lock );
   
   pthread_mutex_lock( &rsc->pending_lock );
   curl_easy_setopt( rsc->curl_h, CURLOPT_HTTPPOST, NULL );

   // remove the head
   ReplicaUpload* ru = (*rsc->pending)[0];
   rsc->pending->erase( rsc->pending->begin() );

   pthread_mutex_unlock( &rsc->pending_lock );
   pthread_mutex_unlock( &this->download_lock );

   ru->status[rsc->id] = status;

   dbprintf("replicate %s to %s rc = %d\n", ru->path, rsc->url, status );

   this->unref_replica_upload( ru );
}

// start the next replica
int ReplicaUploader::start_next_replica( struct replica_server_channel* rsc ) {
   pthread_mutex_lock( &this->download_lock );
   pthread_mutex_lock( &rsc->pending_lock );

   if( rsc->pending->size() > 0 ) {
      ReplicaUpload* ru = (*rsc->pending)[0];

      curl_easy_setopt( rsc->curl_h, CURLOPT_HTTPPOST, ru->form_data );
   }

   pthread_mutex_unlock( &rsc->pending_lock );
   pthread_mutex_unlock( &this->download_lock );
   
   return 0;
}


// enqueue a replica upload.
// return a referenced ReplicaUpload
ReplicaUpload* ReplicaUploader::start_replica_block( struct fs_file_handle* fh, char const* data_root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version, int64_t mtime_sec, int32_t mtime_nsec ) {
   
   if( this->num_replica_servers == 0 )
      return 0;
      
   ReplicaUpload* ru = new ReplicaUpload( this->conf );
   int rc = ru->setup_block( fh, data_root, fs_path, file_version, block_id, block_version, mtime_sec, mtime_nsec, this->conf->blocking_factor, this->conf->verify_peer );
   if( rc != 0 ) {
      delete ru;
      return NULL;
   }
   else {
      ru->ref();

      // enqueue this RU on all replica servers
      for( int i = 0; i < this->num_replica_servers; i++ ) {
         this->enqueue_replica( &this->replica_servers[i], ru );
      }
      
      return ru;
   }
}


// enqueue a replica upload for a manifest
ReplicaUpload* ReplicaUploader::start_replica_manifest( struct fs_file_handle* fh, char* manifest_str, size_t manifest_len, char const* fs_path, int64_t file_version, int64_t mtime_sec, int32_t mtime_nsec ) {

   if( this->num_replica_servers == 0 )
      return 0;

   ReplicaUpload* ru = new ReplicaUpload( this->conf );
   int rc = ru->setup_manifest( fh, manifest_str, manifest_len, fs_path, file_version, mtime_sec, mtime_nsec, this->conf->verify_peer );
   if( rc != 0 ) {
      delete ru;
      return NULL;
   }
   else {
      ru->ref();

      // enqueue this RU on all replica servers
      for( int i = 0; i < this->num_replica_servers; i++ ) {
         this->enqueue_replica( &this->replica_servers[i], ru );
      }

      return ru;
   }
}


// main loop for our replicator
void* ReplicaUploader::thread_main( void* arg ) {
   ReplicaUploader* ctx = (ReplicaUploader*)arg;

   ctx->stopped = false;
   
   dbprintf("%s", " started\n");
   while( ctx->running ) {
      int rc = 0;

      pthread_mutex_lock( &ctx->download_lock );
      
      // do download/upload
      rc = ctx->process_curl( 0 );
      if( rc != 0 ) {
         errorf( "process_curl rc = %d\n", rc );
         rc = 0;
      }

      pthread_mutex_unlock( &ctx->download_lock );
      
      // accumulate finished replicas 
      vector<struct replica_server_channel*> rscs;
      struct replica_server_channel* rsc = NULL;
      
      do {
         rsc = ctx->next_ready_server( &rc );

         if( rsc ) {
            ctx->finish_replica( rsc, rc );
            rscs.push_back( rsc );
         }
      } while( rsc != NULL );
      
      // start up the next replicas
      for( vector<struct replica_server_channel*>::iterator itr = rscs.begin(); itr != rscs.end(); itr++ ) {
         ctx->start_next_replica( *itr );
      }
   }

   dbprintf("%s", " exited\n");
   ctx->stopped = true;
   return NULL;
}

int ReplicaUploader::start() {
   this->running = true;
   this->thread = md_start_thread( ReplicaUploader::thread_main, this, false );
   if( this->thread < 0 )
      return this->thread;
   else
      return 0;
}

int ReplicaUploader::cancel() {
   this->running = false;
   return 0;
}


// start up replication
int replication_init( struct md_syndicate_conf* conf ) {
   rutp = new ReplicaUploader( conf );
   rutp->start();
   return 0;
}

// shut down replication
int replication_shutdown() {
   rutp->cancel();
   delete rutp;
   return 0;
}


// replicate a sequence of modified blocks, and the associated manifest
// fh must be write-locked
// fh->fent must be read-locked
int fs_entry_replicate_write( struct fs_core* core, struct fs_file_handle* fh, modification_map* modified_blocks, bool sync ) {
   
   char* fs_path = fh->path;
   struct fs_entry* fent = fh->fent;
   
   // don't even bother if there are no replica servers
   if( rutp->get_num_replica_servers() == 0 )
      return 0;
   
   // replicate the manifest
   char* manifest_data = NULL;
   int rc = 0;
   ssize_t manifest_len = fs_entry_serialize_manifest( core, fent, &manifest_data );

   vector<struct ReplicaUpload*> replicas;

   if( manifest_len > 0 ) {
      ReplicaUpload* ru_manifest = rutp->start_replica_manifest( fh, manifest_data, manifest_len, fs_path, fent->version, fent->mtime_sec, fent->mtime_nsec );
      replicas.push_back( ru_manifest );
   }
   else {
      errorf( "fs_entry_serialize_manifest(%s) rc = %zd\n", fs_path, manifest_len );
      return rc;
   }
   
   // start replicating all modified blocks.  They could be local or staging, so we'll need to be careful 
   for( modification_map::iterator itr = modified_blocks->begin(); itr != modified_blocks->end(); itr++ ) {
      char* data_root = core->conf->data_root;

      if( !URL_LOCAL( fent->url ) ) {
         data_root = core->conf->staging_root;
      }
      
      ReplicaUpload* ru_block = rutp->start_replica_block( fh, data_root, fs_path, fent->version, itr->first, itr->second, fent->mtime_sec, fent->mtime_nsec );
      replicas.push_back( ru_block );
   }

   // if we're supposed to wait, then wait
   if( sync ) {
      pthread_mutex_t* wait_locks = CALLOC_LIST( pthread_mutex_t, replicas.size() );

      // wait until the reference count of each RU becomes 1 again
      for( unsigned int i = 0; i < replicas.size(); i++ ) {
         pthread_mutex_init( &wait_locks[i], NULL );
         pthread_mutex_lock( &wait_locks[i] );
         replicas[i]->wait_ref( 1, &wait_locks[i] );
      }

      for( unsigned int i = 0; i < replicas.size(); i++ ) {
         pthread_mutex_lock( &wait_locks[i] );

         int refs = rutp->unref_replica_upload( replicas[i] );
         if( refs == 0 ) {
            replicas[i] = NULL;
         }
         
         pthread_mutex_unlock( &wait_locks[i] );
         pthread_mutex_destroy( &wait_locks[i] );
      }

      free( wait_locks );
   }
   else {
      // otherwise, don't.  Release references; let the replica thread handle memory
      for( unsigned int i = 0; i < replicas.size(); i++ ) {
         int ref = rutp->unref_replica_upload( replicas[i] );
         if( ref == 0 ) {
            replicas[i] = NULL;
         }
      }
   }

   return rc;
}

int fs_entry_replicate_wait( struct fs_file_handle* fh ) {
   // wait until the ref count becomes 0
   while( true ) {
      int ref_count = rutp->get_running( fh );
      if( ref_count > 0 ) {
         // not done yet--check back later
         usleep(1000);
      }
      else {
         break;
      }
   }

   return 0;
}