/*
   Copyright 2013 The Trustees of Princeton University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "libsyndicate/storage.h"

// load a file as a string.  return the buffer with the file on success, or NULL on error
char* md_load_file_as_string( char const* path, size_t* size ) {
   
   off_t size_or_error = 0;
   char* buf = md_load_file( path, &size_or_error );

   if( buf == NULL ) {
      SG_error("md_load_file('%s') rc = %d\n", path, (int)size_or_error );
      return NULL;
   }

   *size = size_or_error;
   
   char* ret = (char*)realloc( buf, *size + 1 );
   if( ret == NULL ) {
      SG_safe_free( buf );
      return NULL;
   }
   
   ret[ *size ] = 0;

   return ret;
}  


// safely load secret information as a null-terminated string, ensuring that the memory allocated is mlocked
// return 0 on success
// return negative errno on stat(2) failure on path
// return -ENODATA if we failed to allocate a buffer of sufficient size for the file referred to by the path 
// return -ENODATA if we failed to open path for reading, or failed to read all of the file
// return -EINVAL if the path does not refer to a regular file (or a symlink to a regular file)
// return -EOVERFLOW if the buf was allocated, but does not contain sufficient space
int md_load_secret_as_string( struct mlock_buf* buf, char const* path ) {
   
   struct stat statbuf;
   int rc = 0;
   
   rc = stat( path, &statbuf );
   if( rc != 0 ) {
      
      rc = -errno;
      SG_error("stat(%s) errno = %d\n", path, rc );
      return rc;
   }
   
   if( !S_ISREG( statbuf.st_mode ) ) {
      
      return -EINVAL;
   }
   
   bool alloced = false;
   
   if( buf->ptr == NULL ) {
      
      rc = mlock_calloc( buf, statbuf.st_size + 1 );
      if( rc != 0 ) {
         
         SG_error("mlock_calloc rc = %d\n", rc );
         return -ENODATA;
      }
      
      alloced = true;
   }
   else if( buf->len <= (size_t)statbuf.st_size ) {
      
      SG_error("insufficient space for %s\n", path );
      return -EOVERFLOW;
   }
   
   FILE* f = fopen( path, "r" );
   if( f == NULL ) {
      rc = -errno;
      
      if( alloced ) {
         mlock_free( buf );
      }
      
      SG_error("fopen(%s) rc = %d\n", path, rc );
      return -ENODATA;
   }
   
   buf->len = fread( buf->ptr, 1, statbuf.st_size, f );
   fclose( f );
   
   if( buf->len != (unsigned)statbuf.st_size ) {
      
      SG_error("Read only %zu of %zu bytes\n", buf->len, statbuf.st_size );
      
      if( alloced ) {
         mlock_free( buf );
      }
      
      return -ENODATA;
   }

   // null-terminate
   char* char_ptr = (char*)buf->ptr;
   char_ptr[ buf->len ] = 0;

   return 0;
}  


// initialize local storage
// return 0 on success
// return -ENOMEM if OOM
// return negative on storage-related error (md_mkdirs)
int md_init_local_storage( struct md_syndicate_conf* c ) {
   
   char cwd[PATH_MAX + 1];
   char path[PATH_MAX + 1];
   memset(cwd, 0, PATH_MAX + 1 );
   
   int rc = 0;
   
   // if storage root is NULL, then it's the directory name of the config file 
   if( c->storage_root == NULL ) {
       
       c->storage_root = md_dirname( c->config_file_path, NULL );
       if( c->storage_root == NULL ) {
           return -ENOMEM;
       }
   }
   
   // if data root is NULL, then it's "~/.syndicate/data"
   if( c->data_root == NULL ) {
       
       char* config_dir = NULL;
       size_t config_dir_len = 0;
       
       rc = md_expand_path( SG_DEFAULT_CONFIG_DIR, &config_dir, &config_dir_len );
       if( rc != 0 ) {
           return rc;
       }
       
       c->data_root = md_fullpath( config_dir, "data", NULL );
       if( c->data_root == NULL ) {
           return -ENOMEM;
       }
   }
   
   char** dirs[] = {
      &c->storage_root,
      &c->data_root,
      &c->volumes_path,
      &c->gateways_path,
      &c->users_path,
      &c->drivers_path,
      &c->logs_path,
      &c->syndicate_path,
      (char**)NULL
   };
   
   // NOTE: matches names of dirs
   char const* dir_names[] = {
      "storage",
      "data",
      "volumes",
      "gateways",
      "users",
      "drivers",
      "logs",
      "syndicate",
      (char const*)NULL
   };
   
   // NOTE: must be an absolute path 
   md_dirname( c->config_file_path, cwd );
   
   // expand to full path, and make each directory 
   for( int i = 0; dirs[i] != NULL; i++ ) {
      
      char** dirp = dirs[i];
      
      if( *(dirp) == NULL ) {
         SG_error("Configuration is missing the path to the '%s' directory.  Set it in %s with '%s='\n", dir_names[i], c->config_file_path, dir_names[i]);
         rc = -EINVAL;
         break;
      }
      
      if( **dirp != '/' ) {
         
         md_fullpath( cwd, *dirp, path );
         
         SG_safe_free( *dirp );
         *dirp = SG_strdup_or_null( path );
         
         if( *dirp == NULL ) {
            rc = -ENOMEM;
            break;
         }
      }
      else {
         
         strcpy( path, *dirp );
      }
      
      rc = md_mkdirs( path );
      if( rc != 0 ) {
         SG_error("md_mkdirs('%s') rc = %d\n", path, rc );
         break;
      }
   }
   
   return rc;
}

// recursively make a directory.
// return 0 if the directory exists at the end of the call.
// return -ENOMEM if OOM
// return negative if the directory could not be created.
int md_mkdirs2( char const* dirp, int start, mode_t mode ) {
   
   unsigned int i = start;
   struct stat statbuf;
   int rc = 0;
   char* currdir = SG_CALLOC( char, strlen(dirp) + 1 );
   
   if( currdir == NULL ) {
      return -ENOMEM;
   }
   
   while( i <= strlen(dirp) ) {
      
      if( dirp[i] == '/' || i == strlen(dirp) ) {
         
         strncpy( currdir, dirp, i == 0 ? 1 : i );
         
         rc = stat( currdir, &statbuf );
         if( rc == 0 && !S_ISDIR( statbuf.st_mode ) ) {
            
            SG_safe_free( currdir );
            return -EEXIST;
         }
         if( rc != 0 ) {
            
            rc = mkdir( currdir, mode );
            if( rc != 0 ) {
               
               rc = -errno;
               SG_safe_free(currdir);
               return rc;
            }
         }
      }
      
      i++;
   }
   
   SG_safe_free(currdir);
   return 0;
}

int md_mkdirs3( char const* dirp, mode_t mode ) {
   return md_mkdirs2( dirp, 0, mode );
}

int md_mkdirs( char const* dirp ) {
   return md_mkdirs2( dirp, 0, 0755 );
}

// remove a bunch of empty directories
// return 0 on success 
// return -ENOMEM on OOM
// return negative on error from rmdir(2)
int md_rmdirs( char const* dirp ) {
   
   char* dirname = SG_strdup_or_null( dirp );
   if( dirname == NULL ) {
      return -ENOMEM;
   }
   
   char* dirname_buf = SG_CALLOC( char, strlen(dirp) + 1 );
   if( dirname_buf == NULL ) {
      
      SG_safe_free( dirname );
      return -ENOMEM;
   }
   
   int rc = 0;
   
   while( strlen(dirname) > 0 ) {
      
      rc = rmdir( dirname );
      if( rc != 0 ) {
         
         rc = -errno;
         break;
      }
      else {
         
         md_dirname( dirname, dirname_buf );
         strcpy( dirname, dirname_buf );
      }
   }
   
   SG_safe_free( dirname );
   SG_safe_free( dirname_buf );
   return rc;
}

// get the path to a certificate 
// path must be long enough--PATH_MAX should be safe--but it will be truncated with '\0'
void md_object_cert_path( char const* object_dir, char const* object_name, char* path, size_t path_len ) {
   
   snprintf( path, path_len-1, "%s/%s.cert", object_dir, object_name );
   path[ path_len-1 ] = '\0';
}


// load a syndicate public key from disk 
// return 0 on success, and set *key to include the public key 
// return -errno on filesystem-related errors 
int md_syndicate_pubkey_load( char const* syndicate_dir, char const* syndicate_name, char** syndicate_pubkey_pem, size_t* syndicate_pubkey_pem_len ) {
    
   char path[ PATH_MAX+1 ];
   char* data = NULL;
   off_t data_len = 0;
   
   snprintf( path, PATH_MAX-1, "%s/%s.pub", syndicate_dir, syndicate_name );
   
   data = md_load_file( path, &data_len );
   if( data_len < 0 ) {
       
       SG_error("md_load_file('%s') rc = %d\n", path, (int)data_len);
       return (int)data_len;
   }
   
   *syndicate_pubkey_pem = data;
   *syndicate_pubkey_pem_len = data_len;
   return 0;
}


// store a syndicate public key to disk 
// return 0 on success 
// return -errno on filesystem-related errors 
int md_syndicate_pubkey_store( char const* syndicate_dir, char const* syndicate_name, char* syndicate_pubkey_pem, size_t syndicate_pubkey_pem_len ) {
    
    int rc = 0;
    char path[PATH_MAX+1];
    
    snprintf( path, PATH_MAX-1, "%s/%s.pub", syndicate_dir, syndicate_name );
    rc = md_write_file( path, syndicate_pubkey_pem, syndicate_pubkey_pem_len, 0644 );
    if( rc < 0 ) {
        
        SG_error("md_write_file('%s') rc = %d\n", path, rc );
        return rc;
    }
    
    return 0;
}

// load a volume cert from disk 
// return 0 on success, and populate *volume_cert 
// return -errno on filesystem-related errors 
int md_volume_cert_load( char const* volumes_dir, char const* volume_name, ms::ms_volume_metadata* volume_cert ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   char* data = NULL;
   off_t data_len = 0;
   
   md_object_cert_path( volumes_dir, volume_name, path, PATH_MAX );
   
   data = md_load_file( path, &data_len );
   if( data_len < 0 ) {
      
      SG_error("md_load_file('%s') rc = %d\n", path, (int)data_len);
      return (int)data_len;
   }
   
   rc = md_parse< ms::ms_volume_metadata >( volume_cert, data, data_len );
   SG_safe_free( data );
   
   if( rc != 0 ) {
      
      SG_error("md_parse< ms::ms_volume_metadata >('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   return rc;
}

// store a volume cert from disk 
// return 0 on success
// return -errno on filesystem-related errors 
int md_volume_cert_store( char const* volumes_dir, char const* volume_name, ms::ms_volume_metadata* volume_cert ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   char* buf = NULL;
   size_t buf_len = 0;
   
   md_object_cert_path( volumes_dir, volume_name, path, PATH_MAX );
   
   rc = md_serialize< ms::ms_volume_metadata >( volume_cert, &buf, &buf_len );
   if( rc != 0 ) {
      
      SG_error("md_serialize rc = %d\n", rc );
      return rc;
   }
   
   rc = md_write_file( path, buf, buf_len, 0640 );
   
   SG_safe_free( buf );
   
   if( rc < 0 ) {
      
      SG_error("md_write_file('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   return 0;
}


// remove a volume cert from disk 
// return 0 on success
// return -errno on filesystem-related erorrs 
int md_volume_cert_remove( char const* volumes_dir, char const* volume_name ) {
   
   int rc = 0;
   char path[PATH_MAX+1];
   
   md_object_cert_path( volumes_dir, volume_name, path, PATH_MAX );
   
   rc = unlink( path );
   if( rc != 0 ) {
      
      rc = -errno;
      if( rc != -ENOENT ) {
         
         SG_error("unlink('%s') rc = %d\n", path, rc );
      }
   }
   
   return rc;
}


// get a cached gateway certificate 
// return 0 on success, and populate *cert on success 
// return -ENOMEM on OOM
// return -ENOENT if not found
// return other -errno related to filesystem errors 
int md_gateway_cert_load( char const* gateways_root, char const* gateway_name, ms::ms_gateway_cert* cert ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   char* data = NULL;
   off_t data_len = 0;
   
   md_object_cert_path( gateways_root, gateway_name, path, PATH_MAX );
   
   data = md_load_file( path, &data_len );
   if( data_len < 0 ) {
      
      SG_error("md_load_file('%s') rc = %d\n", path, (int)data_len);
      return (int)data_len;
   }
   
   rc = md_parse< ms::ms_gateway_cert >( cert, data, data_len );
   SG_safe_free( data );
   
   if( rc != 0 ) {
      
      SG_error("md_parse< ms::ms_gateway_cert >('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   return 0;
}


// store a gateway cert from disk
// return 0 on success
// return -errno on filesystem-related errors 
int md_gateway_cert_store( char const* gateways_dir, char const* gateway_name, ms::ms_gateway_cert* gateway_cert ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   char* buf = NULL;
   size_t buf_len = 0;
   
   md_object_cert_path( gateways_dir, gateway_name, path, PATH_MAX );
   
   rc = md_serialize< ms::ms_gateway_cert >( gateway_cert, &buf, &buf_len );
   if( rc != 0 ) {
      
      SG_error("md_serialize rc = %d\n", rc );
      return rc;
   }
   
   rc = md_write_file( path, buf, buf_len, 0640 );
   
   SG_safe_free( buf );
   
   if( rc < 0 ) {
      
      SG_error("md_write_file('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   return 0;
}


// remove a gateway cert from disk 
// return 0 on success
// return -errno on filesystem-related erorrs 
int md_gateway_cert_remove( char const* gateways_dir, char const* gateway_name ) {
   
   int rc = 0;
   char path[PATH_MAX+1];
   
   md_object_cert_path( gateways_dir, gateway_name, path, PATH_MAX );
   
   rc = unlink( path );
   if( rc != 0 ) {
      
      rc = -errno;
      if( rc != -ENOENT ) {
         
         SG_error("unlink('%s') rc = %d\n", path, rc );
      }
   }
   
   return rc;
}

// get a gateway's private key from disk 
// return 0 on success, and populate *private_key 
// return -ENOMEM on OOM 
// return other -errno related to filesystem errors
int md_gateway_private_key_load( char const* gateways_root, char const* gateway_name, struct mlock_buf* private_key ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   
   memset( path, 0, PATH_MAX+1 );
   snprintf( path, PATH_MAX-1, "%s/%s.pkey", gateways_root, gateway_name );
   
   rc = md_load_secret_as_string( private_key, path );
   if( rc != 0 ) {
      
      SG_error("md_load_secret_as_string('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   return rc;
}


// load a cached user cert from disk 
// return 0 on success
// return -ENOMEM on OOM 
// return -errno on filesystem-related errors
int md_user_cert_load( char const* users_root, char const* username, ms::ms_user_cert* user_cert ) {
   
   int rc = 0;
   char* data = NULL;
   long data_len = 0;
   char path[ PATH_MAX+1 ];
   
   md_object_cert_path( users_root, username, path, PATH_MAX );
   
   data = md_load_file( path, &data_len );
   if( data_len < 0 || data == NULL ) {
      
      SG_error("md_load_file('%s') rc = %d\n", path, (int)data_len);
      return (int)data_len;
   }
   
   rc = md_parse< ms::ms_user_cert >( user_cert, data, data_len );
   SG_safe_free( data );
   
   if( rc != 0 ) {
      SG_error("Failed to load user cert '%s'\n", path );
      return rc;
   }
   
   return 0;
}


// store a user cert from disk 
// return 0 on success
// return -errno on filesystem-related errors 
int md_user_cert_store( char const* users_dir, char const* username, ms::ms_user_cert* user_cert ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   char* buf = NULL;
   size_t buf_len = 0;
   
   md_object_cert_path( users_dir, username, path, PATH_MAX );

   SG_debug("store user cert '%s'\n", path );
   
   rc = md_serialize< ms::ms_user_cert >( user_cert, &buf, &buf_len );
   if( rc != 0 ) {
      
      SG_error("md_serialize rc = %d\n", rc );
      return rc;
   }
   
   rc = md_write_file( path, buf, buf_len, 0640 );
   
   SG_safe_free( buf );
   
   if( rc < 0 ) {
      
      SG_error("md_write_file('%s') rc = %d\n", path, rc );
      return rc;
   }
   
   return 0;
}


// remove a user cert from disk 
// return 0 on success
// return -errno on filesystem-related erorrs 
int md_user_cert_remove( char const* users_dir, char const* username ) {
   
   int rc = 0;
   char path[PATH_MAX+1];
   
   md_object_cert_path( users_dir, username, path, PATH_MAX );
   SG_debug("unlink user cert '%s'\n", path );
   
   rc = unlink( path );
   if( rc != 0 ) {
      
      rc = -errno;
      if( rc != -ENOENT ) {
         
         SG_error("unlink('%s') rc = %d\n", path, rc );
      }
   }
   
   return rc;
}


// load the cached cert bundle version 
// return 0 on success 
// return -EPERM on failure
// return -ENOMEM on OOM 
// return -errno on filesystem-related error 
int md_cert_bundle_version_load( char const* volumes_root, char const* volume_name, uint64_t* cert_bundle_version ) {
   
   char path[ PATH_MAX+1 ];
   char* data = NULL;
   long data_len = 0;
   uint64_t ret = 0;
   char* tmp = NULL;
   
   snprintf( path, PATH_MAX-1, "%s/%s.version", volumes_root, volume_name );
   path[ PATH_MAX ] = '\0';
   
   data = md_load_file( path, &data_len );
   if( data_len < 0 ) {
      
      SG_error("md_load_file('%s') rc = %d\n", path, (int)data_len);
      return (int)data_len;
   }
   
   ret = (uint64_t)strtoull( data, &tmp, 10 );
   if( ret == 0 && (*tmp != '\0' && *tmp != '\n') ) {
      SG_error("Invalid cert bundle version in '%s'\n", path );
      return -EPERM;
   }
   
   *cert_bundle_version = ret;
   return 0;
}


// store the cert bundle version
// return 0 on succes 
// return -errno on filesystem-related error 
int md_cert_bundle_version_store( char const* volumes_root, char const* volume_name, uint64_t cert_bundle_version ) {
   
   int rc = 0;
   char path[ PATH_MAX+1 ];
   char data_buf[50];
   
   snprintf( path, PATH_MAX-1, "%s/%s.version", volumes_root, volume_name );
   path[ PATH_MAX ] = '\0';
   
   snprintf( data_buf, 49, "%" PRIu64, cert_bundle_version );
   data_buf[49] = '\0';
   
   rc = md_write_file( path, data_buf, strlen(data_buf), 0640 );
   if( rc < 0 ) {
      
      SG_error("md_write_file('%s') rc = %d\n", path, rc );
   }
   
   return rc;
}

