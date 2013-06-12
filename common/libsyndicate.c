/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/


#include "libsyndicate.h"

int _DEBUG_SYNDICATE = 1;
int _ERROR_SYNDICATE = 1;

static struct md_syndicate_conf CONF;

static unsigned long _connect_timeout = 30L;
static unsigned long _transfer_timeout = 60L;     // 1 minute
static int _signals = 1;

static struct md_path_locks md_locks;

static struct md_user_entry md_guest_user;

char const* MD_HTTP_NOMSG = "\n";
char const* MD_HTTP_200_MSG = "OK\n";
char const* MD_HTTP_400_MSG = "Bad Request\n";
char const* MD_HTTP_401_MSG = "Invalid authorization credentials\n";
char const* MD_HTTP_403_MSG = "Credentials required\n";
char const* MD_HTTP_404_MSG = "Not found\n";
char const* MD_HTTP_409_MSG = "Operation conflict\n";
char const* MD_HTTP_413_MSG = "Requested entry too big\n";
char const* MD_HTTP_422_MSG = "Unprocessable entry\n";
char const* MD_HTTP_500_MSG = "Internal Server Error\n";
char const* MD_HTTP_501_MSG = "Not implemented\n";
char const* MD_HTTP_504_MSG = "Remote Server Timeout\n";

char const* MD_HTTP_DEFAULT_MSG = "RESPONSE\n";

// initialize all global data structures
static int md_runtime_init( int gateway_type, struct md_syndicate_conf* c, char const* mountpoint ) {

   GOOGLE_PROTOBUF_VERIFY_VERSION;

   md_path_locks_create( &md_locks );
   
   md_guest_user.uid = MD_GUEST_UID;
   md_guest_user.username = strdup( "GUEST" );
   md_guest_user.password_hash = NULL;
   
   // get the umask
   mode_t um = get_umask();
   c->usermask = um;
   
   int rc = 0;
   
   // mountpoint
   if( mountpoint ) {
      c->mountpoint = strdup( mountpoint );
   }
   
   // gateway server without a read url.  generate one
   struct addrinfo hints;
   memset( &hints, 0, sizeof(hints) );
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = AI_NUMERICSERV | AI_CANONNAME;
   hints.ai_protocol = 0;
   hints.ai_canonname = NULL;
   hints.ai_addr = NULL;
   hints.ai_next = NULL;

   char portnum_buf[10];
   sprintf(portnum_buf, "%d", c->portnum);

   struct addrinfo *result = NULL;
   char hostname[HOST_NAME_MAX+1];
   gethostname( hostname, HOST_NAME_MAX );

   rc = getaddrinfo( hostname, portnum_buf, &hints, &result );
   if( rc != 0 ) {
      // could not get addr info
      errorf("getaddrinfo: %s\n", gai_strerror( rc ) );
      return -abs(rc);
   }
   
   // now reverse-lookup ourselves
   char hn[HOST_NAME_MAX+1];
   rc = getnameinfo( result->ai_addr, result->ai_addrlen, hn, HOST_NAME_MAX, NULL, 0, NI_NAMEREQD );
   if( rc != 0 ) {
      errorf("getnameinfo: %s\n", gai_strerror( rc ) );
      return -abs(rc);
   }
   
   c->hostname = strdup(hn);
   
   dbprintf("canonical hostname is %s\n", hn);
   
   // create a public url, if it does not exist
   if( c->content_url == NULL ) {
      c->content_url = CALLOC_LIST( char, strlen(c->hostname) + 20 );
      sprintf(c->content_url, "http://%s:%d/", c->hostname, c->portnum );
      dbprintf("content URL is %s\n", c->content_url );
   }

   // make sure we have data root and staging root
   char* cwd = getenv( "HOME" );
   char* data_root = md_fullpath( cwd, ".syndicate/data", NULL );
   char* staging_root = md_fullpath( cwd, ".syndicate/staging", NULL );
   char* logfile_path = md_fullpath( cwd, ".syndicate/access.log", NULL );
   char* replica_logfile_path = md_fullpath( cwd, ".syndicate/replica.log", NULL );
   char* RG_metadata_path = md_fullpath( cwd, ".syndicate/RG-metadata", NULL );
   char* AG_metadata_path = md_fullpath( cwd, ".syndicate/AG-metadata", NULL );
   
   if( c->data_root == NULL )
      c->data_root = strdup( data_root );

   if( c->staging_root == NULL )
      c->staging_root = strdup( staging_root );

   if( c->logfile_path == NULL )
      c->logfile_path = strdup( logfile_path );

   if( c->replica_logfile == NULL )
      c->replica_logfile = strdup( replica_logfile_path );

   if( gateway_type == SYNDICATE_AG && c->gateway_metadata_root == NULL )
      c->gateway_metadata_root = strdup( AG_metadata_path );

   if( gateway_type == SYNDICATE_RG && c->gateway_metadata_root == NULL )
      c->gateway_metadata_root = strdup( RG_metadata_path );
      
   free( data_root );
   free( staging_root );
   free( logfile_path );
   free( replica_logfile_path );
   free( AG_metadata_path );
   free( RG_metadata_path );

   if( gateway_type == SYNDICATE_UG ) {
      dbprintf("data root:     %s\n", c->data_root );
      dbprintf("staging root:  %s\n", c->staging_root );
      dbprintf("replica log:   %s\n", c->replica_logfile );
   }
   else {
      dbprintf("metadata root: %s\n", c->gateway_metadata_root );
   }

   dbprintf("access log:    %s\n", c->logfile_path );

   // make sure the storage roots exist
   if( rc == 0 ) {
      const char* dirs[] = {
         c->data_root,
         c->staging_root,
         NULL
      };

      for( int i = 0; dirs[i] != NULL; i++ ) {         
         rc = md_mkdirs( dirs[i] );
         if( rc != 0 ) {
            errorf("md_mkdirs(%s) rc = %d\n", dirs[i], rc );
            return rc;
         }
      }
   }

   memcpy( &CONF, c, sizeof(CONF) );

   return rc;
}

int md_debug( int level ) {
   int prev = _DEBUG_SYNDICATE;
   _DEBUG_SYNDICATE = level;
   return prev;
}

int md_error( int level ) {
   int prev = _ERROR_SYNDICATE;
   _ERROR_SYNDICATE = level;
   return prev;
}


// shut down the library.
// free all global data structures
int md_shutdown() {
   md_path_locks_free( &md_locks );

   // shut down protobufs
   google::protobuf::ShutdownProtobufLibrary();

   return 0;
}


// read a configuration line, with the following syntax:
// KEY = "VALUE_1" "VALUE_2" ... "VALUE_N".  Arbitrarily many spaces are allowed.  
// Quotes within quotes are allowed, as long as they are escaped
// return number of values on success (0 for comments and empty lines), negative on syntax/parse error.
// Pass NULL for key and values if you want them allocated for you (otherwise, they must be big enough)
// key and value will remain NULL if the line was a comment.
int md_read_conf_line( char* line, char** key, char*** values ) {
   char* line_cpy = strdup( line );
   
   // split on '='
   char* key_half = strtok( line_cpy, "=" );
   if( key_half == NULL ) {
      // ensure that this is a comment or an empty line
      char* key_str = strtok( line_cpy, " \n" );
      if( key_str == NULL ) {
         free( line_cpy );    // empty line
         return 0;
      }
      else if( key_str[0] == COMMENT_KEY ) {
         free( line_cpy );    // comment
         return 0;
      }
      else {
         free( line_cpy );    // bad line
         return -1;
      }
   }
   
   char* value_half = line_cpy + strlen(key_half) + 1;
   
   char* key_str = strtok( key_half, " \t" );
   
   // if this is a comment, then skip it
   if( key_str[0] == COMMENT_KEY ) {
      free( line_cpy );
      return 0;     
   }
   
   // if this is a newline, then skip it
   if( key_str[0] == '\n' || key_str[0] == '\r' ) {
      free( line_cpy );
      return 0;      // just a blank line
   }
   
   // read each value
   vector<char*> val_list;
   char* val_str = strtok( value_half, " \n" );
   
   while( 1 ) {
      if( val_str == NULL )
         // no more values
         break;
      
      // got a value string
      if( val_str[0] != '"' ) {
         // needs to be in quotes!
         free( line_cpy );
         return -3;
      }
      
      // advance beyond the first '"'
      val_str++;
      
      // scan until we find an unescaped "
      int escaped = 0;
      int end = -1;
      for( int i = 0; i < (signed)strlen(val_str); i++ ) {
         if( val_str[i] == '\\' ) {
            escaped = 1;
            continue;
         }
         
         if( escaped ) {
            escaped = 0;
            continue;
         }
         
         if( val_str[i] == '"' ) {
            end = i;
            break;
         }
      }
      
      if( end == -1 ) {
         // the string didn't end in a "
         free( line_cpy );
         return -4;
      }
      
      val_str[end] = 0;
      val_list.push_back( val_str );
      
      // next value
      val_str = strtok( NULL, " \n" );
   }
   
   // get the key
   if( *key == NULL ) {
      *key = (char*)calloc( strlen(key_str) + 1, 1 );
   }
   
   strcpy( *key, key_str );
   
   // get the values
   if( *values == NULL ) {
      *values = (char**)calloc( sizeof(char*) * (val_list.size() + 1), 1 );
   }
   
   for( int i = 0; i < (signed)val_list.size(); i++ ) {
      (*values)[i] = strdup( val_list.at(i) );
   }
   
   free( line_cpy );
   
   return val_list.size();
}


// read the configuration file and populate a md_syndicate_conf structure
int md_read_conf( char const* conf_path, struct md_syndicate_conf* conf ) {

   FILE* fd = fopen( conf_path, "r" );
   if( fd == NULL ) {
      errorf( "could not read %s\n", conf_path);
      return -1;
   }
   
   memset( conf, 0, sizeof( struct md_syndicate_conf ) );
   
   char buf[4096];    // big enough?
   
   char* eof = NULL;
   int line_cnt = 0;
   
   do {
      memset( buf, 0, sizeof(char) * 4096 );
      eof = fgets( buf, 4096, fd );
      if( eof == NULL )
         break;
         
      line_cnt++;
      
      char* key = NULL;
      char** values = NULL;
      int num_values = md_read_conf_line( buf, &key, &values );
      if( num_values <= 0 ) {
         //dbprintf("read_conf: ignoring malformed line %d\n", line_cnt );
         continue;
      }
      if( key == NULL || values == NULL ) {
         continue;      // comment or empty line
      }
      
      // have key, values.
      // what to do?
      if( strcmp( key, DEFAULT_READ_FRESHNESS_KEY ) == 0 ) {
         // pull time interval
         char *end = NULL;
         long time = strtol( values[0], &end, 10 );
         if( end[0] != '\0' ) {
            errorf( " WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            conf->default_read_freshness = time;
         }
      }
      
      else if( strcmp( key, DEFAULT_WRITE_FRESHNESS_KEY ) == 0 ) {
         // pull time interval
         char *end = NULL;
         long time = strtol( values[0], &end, 10 );
         if( end[0] != '\0' ) {
            errorf( " WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            conf->default_write_freshness = time;
         }
      }
      
      else if( strcmp( key, METADATA_CONNECT_TIMEOUT_KEY ) == 0 ) {
         // read timeout
         char *end;
         long time = strtol( values[0], &end, 10 );
         if( end[0] != '\0' || (end[0] == '\0' && time == 0) ) {
            errorf( " WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            conf->metadata_connect_timeout = time;
         }
      }
      
      else if( strcmp( key, METADATA_USERNAME_KEY ) == 0 ) {
         // metadata server username
         conf->metadata_username = strdup( values[0] );
      }
      
      else if( strcmp( key, METADATA_PASSWORD_KEY ) == 0 ) {
         // metadata server password
         conf->metadata_password = strdup( values[0] );
      }
      else if( strcmp( key, METADATA_UID_KEY ) == 0 ) {
         // metadata server UID
         char *end;
         long v = strtol( values[0], &end, 10 );
         if( end[0] != '\0' ) {
            errorf( "WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            conf->owner = v;
         }
      }
      
      else if( strcmp( key, AUTH_OPERATIONS_KEY ) == 0 ) {
         // what's the policy for authentication?
         int policy = 0;
         if( strcmp( values[0], "none" ) == 0 )
            policy = 0;
         else if( strcmp( values[0], "read" ) == 0 )
            policy = HTTP_AUTHENTICATE_READ;
         else if( strcmp( values[0], "write" ) == 0 )
            policy = HTTP_AUTHENTICATE_WRITE;
         else if( strcmp( values[0], "readwrite" ) == 0 )
            policy = HTTP_AUTHENTICATE_READWRITE;
         
         conf->http_authentication_mode = policy;
      }
      
      else if( strcmp( key, VERIFY_PEER_KEY ) == 0 ) {
         // verify peer?
         char *end;
         long v = strtol( values[0], &end, 10 );
         if( end[0] != '\0' ) {
            errorf( "WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            conf->verify_peer = (v ? true : false);
         }
      }
      
      else if( strcmp( key, METADATA_URL_KEY ) == 0 ) {
         // metadata publisher URL
         conf->metadata_url = strdup( values[0] );
      }
      
      else if( strcmp( key, LOGFILE_PATH_KEY ) == 0 ) {
         // logfile path
         conf->logfile_path = strdup( values[0] );
      }
      
      else if( strcmp( key, CDN_PREFIX_KEY ) == 0 ) {
         // cdn prefix
         conf->cdn_prefix = strdup( values[0] );
      }

      else if( strcmp( key, PROXY_URL_KEY ) == 0 ) {
         // proxy URL
         conf->proxy_url = strdup( values[0] );
      }
      
      else if( strcmp( key, GATHER_STATS_KEY ) == 0 ) {
         // gather statistics?
         char *end;
         long val = strtol( values[0], &end, 10 );
         if( end[0] != '\0' ) {
            errorf( "WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            if( val == 0 )
               conf->gather_stats = false;
            else
               conf->gather_stats = true;
         }
      }

      else if( strcmp( key, NUM_HTTP_THREADS_KEY ) == 0 ) {
         // how big is the HTTP threadpool?
         conf->num_http_threads = (unsigned int)strtol( values[0], NULL, 10 );
      }
      
      else if( strcmp( key, USE_CHECKSUMS_KEY ) == 0 ) {
         // use checksums
         long v = strtol( values[0], NULL, 10 );
         conf->use_checksums = (v != 0 ? true : false);
      }
      
      else if( strcmp( key, DATA_ROOT_KEY ) == 0 ) {
         // data root
         conf->data_root = strdup( values[0] );
         if( conf->data_root[ strlen(conf->data_root)-1 ] != '/' ) {
            char* tmp = md_prepend( conf->data_root, "/", NULL );
            free( conf->data_root );
            conf->data_root = tmp;
         }
      }

      else if( strcmp( key, STAGING_ROOT_KEY ) == 0 ) {
         // data root
         conf->staging_root = strdup( values[0] );
         if( conf->staging_root[ strlen(conf->staging_root)-1 ] != '/' ) {
            char* tmp = md_prepend( conf->staging_root, "/", NULL );
            free( conf->staging_root );
            conf->staging_root = tmp;
         }
      }

      else if( strcmp( key, PIDFILE_KEY ) == 0 ) {
         // pidfile path
         conf->md_pidfile_path = strdup(values[0]);
      }
      
      else if( strcmp( key, SSL_PKEY_KEY ) == 0 ) {
         // server private key
         conf->server_key = strdup( values[0] );
      }
      
      else if( strcmp( key, SSL_CERT_KEY ) == 0 ) {
         // server certificate
         conf->server_cert = strdup( values[0] );
      }
      
      else if( strcmp( key, PORTNUM_KEY ) == 0 ) {
         char *end;
         long val = strtol( values[0], &end, 10 );
         if( end[0] != '\0' || val <= 0 || val >= 65534 ) {
            errorf( "WARN: ignoring bad config line %d: %s\n", line_cnt, buf );
         }
         else {
            conf->portnum = val;
         }
      }

      else if( strcmp( key, CONTENT_URL_KEY ) == 0 ) {
         // public content URL
         conf->content_url = strdup( values[0] );
         if( conf->content_url[ strlen(conf->content_url)-1 ] != '/' ) {
            char* tmp = md_prepend( conf->content_url, "/", NULL );
            free( conf->content_url );
            conf->content_url = tmp;
         }
      }
            
      else if( strcmp( key, VOLUME_NAME_KEY ) == 0 ) {
         // volume ID
         conf->volume_name = strdup( values[0] );
      }
      
      else if( strcmp( key, GATEWAY_METADATA_KEY ) == 0 ) {
         // replica metadata directory
         conf->gateway_metadata_root = strdup( values[0] );
         if( strlen(conf->gateway_metadata_root) > 0 && conf->gateway_metadata_root[ strlen(conf->gateway_metadata_root) - 1 ] != '/' ) {
            char* tmp = CALLOC_LIST( char, strlen(conf->gateway_metadata_root) + 2 );
            sprintf(tmp, "%s/", conf->gateway_metadata_root );
            free( conf->gateway_metadata_root );
            conf->gateway_metadata_root = tmp;
         }
      }
      
      else if( strcmp( key, REPLICA_URL_KEY ) == 0 ) {
         // replica URL
         if( conf->replica_urls == NULL ) {
            conf->replica_urls = CALLOC_LIST( char*, 2 );
            conf->replica_urls[0] = strdup(values[0]);
         }
         else {
            char** tmp = conf->replica_urls;
            int sz = 0;
            SIZE_LIST( &sz, conf->replica_urls );
            
            conf->replica_urls = CALLOC_LIST( char*, sz + 2 );
            for( int i = 0; i < sz; i++ )
               conf->replica_urls[i] = tmp[i];
            
            conf->replica_urls[sz] = strdup(values[0]);
            free( tmp );
         }
      }
      
      else if( strcmp( key, DEBUG_KEY ) == 0 ) {
         if( strcmp(values[0], "0" ) != 0 )
            md_debug(1);
         else
            md_debug(0);
      }
      
      else if( strcmp( key, DEBUG_READ_KEY ) == 0 ) {
         if( strcmp(values[0], "0" ) != 0 )
            conf->debug_read = 1;
         else
            conf->debug_read = 0;
      }

      else if( strcmp( key, DEBUG_LOCK_KEY ) == 0 ) {
         if( strcmp(values[0], "0" ) != 0 )
            conf->debug_lock = 1;
         else
            conf->debug_lock = 0;
      }
      
      else if( strcmp( key, NUM_REPLICA_THREADS_KEY ) == 0 ) {
         conf->num_replica_threads = (unsigned int)strtol( values[0], NULL, 10 );
      }
      
      else if( strcmp( key, REPLICA_LOGFILE_KEY ) == 0 ) {
         conf->replica_logfile = strdup(values[0]);
      }

      else if( strcmp( key, BLOCKING_FACTOR_KEY ) == 0 ) {
         conf->blocking_factor = (unsigned long)strtol( values[0], NULL, 10 );
      }

      else if( strcmp( key, REPLICA_OVERWRITE_KEY ) == 0 ) {
         conf->replica_overwrite = (strtol(values[0], NULL, 10) != 0 ? true : false);
      }

      else if( strcmp( key, HTTPD_PORTNUM_KEY ) == 0 ) {
         conf->httpd_portnum = strtol( values[0], NULL, 10 );
      }

      else if( strcmp( key, TRANSFER_TIMEOUT_KEY ) == 0 ) {
         conf->transfer_timeout = strtol( values[0], NULL, 10 );
         _transfer_timeout = conf->transfer_timeout;
      }

      else {
         errorf( "WARN: unrecognized key '%s'\n", key );
      }
      
      // clean up
      free( key );
      for( int i = 0; i < num_values; i++ ) {
         free( values[i] );
      }
      free(values);
      
   } while( eof != NULL );
   
   fclose( fd );

   return 0;
}


// free all memory associated with a server configuration
int md_free_conf( struct md_syndicate_conf* conf ) {
   if( conf->metadata_url ) {
      free( conf->metadata_url );
      conf->metadata_url = NULL;
   }
   if( conf->logfile_path ) {
      free( conf->logfile_path );
      conf->logfile_path = NULL;
   }
   if( conf->content_url ) {
      free( conf->content_url );
      conf->content_url = NULL;
   }
   if( conf->replica_urls ) {
      FREE_LIST( conf->replica_urls );
      conf->replica_urls = NULL;
   }
   if( conf->data_root ) {
      free( conf->data_root );
      conf->data_root = NULL;
   }
   if( conf->staging_root ) {
      free( conf->staging_root );
      conf->staging_root = NULL;
   }
   if( conf->cdn_prefix ) {
      free( conf->cdn_prefix );
      conf->cdn_prefix = NULL;
   }
   if( conf->proxy_url ) {
      free( conf->proxy_url );
      conf->proxy_url = NULL;
   }
   if( conf->metadata_username ) {
      free( conf->metadata_username );
      conf->metadata_username = NULL;
   }
   if( conf->metadata_password ) {
      free( conf->metadata_password );
      conf->metadata_password = NULL;
   }
   if( conf->server_key ) {
      free( conf->server_key );
      conf->server_key = NULL;
   }
   if( conf->server_cert ) {
      free( conf->server_cert );
      conf->server_cert = NULL;
   }
   if( conf->replica_logfile ) {
      free( conf->replica_logfile );
      conf->replica_logfile = NULL;
   }
   if( conf->md_pidfile_path ) {
      free( conf->md_pidfile_path );
      conf->md_pidfile_path = NULL;
   }
   if( conf->gateway_metadata_root ) {
      free( conf->gateway_metadata_root );
      conf->gateway_metadata_root = NULL;
   }
      
   return 0;
}



// destroy an md entry
void md_entry_free( struct md_entry* ent ) {
   if( ent->url ) {
      free( ent->url );
      ent->url = NULL;        
   }
   if( ent->path ) {
      free( ent->path );
      ent->path = NULL;
   }
   if( ent->local_path ) {
      free( ent->local_path );
      ent->local_path = NULL;
   }
   if( ent->checksum ) {
      free( ent->checksum );
      ent->checksum = NULL;
   }
}


// destroy a bunch of md_entries
void md_entry_free_all( struct md_entry** ents ) {
   for( int i = 0; ents[i] != NULL; i++ ) {
      md_entry_free( ents[i] );
      free( ents[i] );
   }
}

// duplicate an md_entry.  Make a new one if given NULL
struct md_entry* md_entry_dup( struct md_entry* src ) {
   struct md_entry* ret = (struct md_entry*)calloc( sizeof(struct md_entry), 1 );
   md_entry_dup2( src, ret );
   return ret;
}


// duplicate an md_entry.  Make a new one if given NULL
void md_entry_dup2( struct md_entry* src, struct md_entry* ret ) {
   // copy non-pointers
   memcpy( ret, src, sizeof(md_entry) );

   ret->path = strdup( src->path );
   ret->url = strdup( src->url );

   if( src->local_path )
      ret->local_path = strdup( src->local_path );
   
   if( src->checksum ) {
      ret->checksum = (unsigned char*)calloc( SHA_DIGEST_LENGTH * sizeof(unsigned char), 1 );
      memcpy( ret->checksum, src->checksum, SHA_DIGEST_LENGTH );
   }
}

// concatenate root (a directory path) with path (a relative path)
char* md_fullpath( char const* root, char const* path, char* dest ) {
   char delim = 0;
   int path_off = 0;
   
   int len = strlen(path) + strlen(root) + 2;
   
   if( strlen(root) > 0 ) {
      size_t root_delim_off = strlen(root) - 1;
      if( root[root_delim_off] != '/' && path[0] != '/' ) {
         len++;
         delim = '/';
      }
      else if( root[root_delim_off] == '/' && path[0] == '/' ) {
         path_off = 1;
      }
   }

   if( dest == NULL )
      dest = (char*)calloc( len, 1 );
   
   memset(dest, 0, len);
   
   strcpy( dest, root );
   if( delim != 0 ) {
      dest[strlen(dest)] = '/';
   }
   strcat( dest, path + path_off );
   
   return dest;
}


// write the directory name of a path to dest.
// if a well-formed path is given, then a string ending in a / is returned
char* md_dirname( char const* path, char* dest ) {
   
   if( dest == NULL ) {
      dest = (char*)calloc( strlen(path) + 1, 1 );
   }
   
   // is this root?
   if( strlen(path) == 0 || strcmp( path, "/" ) == 0 ) {
      strcpy( dest, "/" );
      return dest;
   }
   
   int delim_i = strlen(path);
   if( path[delim_i] == '/' ) {
      delim_i--;
   }
   
   for( ; delim_i >= 0; delim_i-- ) {
      if( path[delim_i] == '/' )
         break;
   }
   
   if( delim_i == 0 && path[0] == '/' ) {
      delim_i = 1;
   }
   
   strncpy( dest, path, delim_i );
   dest[delim_i+1] = '\0';
   return dest;
}

// find the depth of a node in a path.
// the depth of / is 0
// the depth of /foo/bar/baz/ is 3
// the depth of /foo/bar/baz is also 3
int md_depth( char const* path ) {
   int i = strlen(path) - 1;
   
   if( i <= 0 )
      return 0;
   
   if( path[i] == '/' )
      i--;
   
   int depth = 0;
   for( ; i >= 0; i-- )
      if( path[i] == '/' )
         depth++;
   
   return depth;
}


// find the integer offset into a path where the directory name begins
// the inex will be at the last '/'
int md_dirname_end( char const* path ) {
   int delim_i = strlen(path) - 1;
   for( ; delim_i >= 0; delim_i-- ) {
      if( path[delim_i] == '/' )
         break;
   }
   
   return delim_i;
}


// write the basename of a path to dest.
char* md_basename( char const* path, char* dest ) {
   int delim_i = strlen(path) - 1;
   if( delim_i <= 0 ) {
      if( dest == NULL )
         dest = strdup("/");
      else
         strcpy(dest, "/");
      return dest;
   }
   if( path[delim_i] == '/' ) {
      // this path ends with '/', so skip over it if it isn't /
      if( delim_i > 0 ) {
         delim_i--;
      }
   }
   for( ; delim_i >= 0; delim_i-- ) {
      if( path[delim_i] == '/' )
         break;
   }
   delim_i++;
   
   if( dest == NULL ) {
      dest = (char*)calloc( strlen(path) - delim_i + 1, 1 );
   }
   else {
      memset( dest, 0, strlen(path) - delim_i + 1 );
   }
   strncpy( dest, path + delim_i, strlen(path) - delim_i );
   return dest;
}


// find the integer offset into a path where the basename begins.
// the index will be right after the '/'
int md_basename_begin( char const* path ) {
   int delim_i = strlen(path) - 1;
   for( ; delim_i >= 0; delim_i-- ) {
      if( path[delim_i] == '/' )
         break;
   }
   
   return delim_i + 1;
}


// prepend a prefix to a string
char* md_prepend( char const* prefix, char const* str, char* output ) {
   if( output == NULL ) {
      output = (char*)calloc( strlen(prefix) + strlen(str) + 1, 1 );
   }
   sprintf(output, "%s%s", prefix, str );
   return output;
}


// hash a path
long md_hash( char const* path ) {
   locale loc;
   const collate<char>& coll = use_facet<collate<char> >(loc);
   return coll.hash( path, path + strlen(path) );
}


// split a path into its components
int md_path_split( char const* path, vector<char*>* result ) {
   char* tmp = NULL;
   char* path_copy = strdup( path );
   char* ptr = path_copy;

   // does the path start with /?
   if( *ptr == '/' ) {
      result->push_back( strdup("/") );
      ptr++;
   }

   // parse through this path
   while( 1 ) {
      char* next_tok = strtok_r( ptr, "/", &tmp );
      ptr = NULL;

      if( next_tok == NULL )
         break;

      result->push_back( strdup(next_tok) );
   }

   free( path_copy );
   return 0;
}

// make sure paths don't end in /, unless they're root.
void md_sanitize_path( char* path ) {
   if( strcmp( path, "/" ) != 0 ) {
      size_t len = strlen(path);
      if( len > 0 ) {
         if( path[len-1] == '/' ) {
            path[len-1] = '\0';
         }
      }
   }
}


// given a URL, is it hosted locally?
bool md_is_locally_hosted( struct md_syndicate_conf* conf, char const* url ) {
   char* url_host = md_url_hostname( url );
   int url_port = md_portnum_from_url( url );

   char* local_host = md_url_hostname( conf->content_url );
   int local_port = conf->portnum;

   bool ret = false;

   if( strcmp( local_host, url_host ) == 0 ) {
      if( (local_port <= 0 && url_port <= 0) || local_port == url_port ) {
         ret = true;
      }
   }

   free( local_host );
   free( url_host );
   return ret;
}


// recursively make a directory.
int md_mkdirs2( char const* dirp, int start, mode_t mode ) {
   char* currdir = (char*)calloc( strlen(dirp) + 1, 1 );
   unsigned int i = start;
   while( i <= strlen(dirp) ) {
      if( dirp[i] == '/' || i == strlen(dirp) ) {
         strncpy( currdir, dirp, i == 0 ? 1 : i );
         struct stat statbuf;
         int rc = stat( currdir, &statbuf );
         if( rc == 0 && !S_ISDIR( statbuf.st_mode ) ) {
            free( currdir );
            return -EEXIST;
         }
         if( rc != 0 ) {
            rc = mkdir( currdir, mode );
            if( rc != 0 ) {
               free(currdir);
               return -errno;
            }
         }
      }
      i++;
   }
   free(currdir);
   return 0;
}

int md_mkdirs3( char const* dirp, mode_t mode ) {
   return md_mkdirs2( dirp, 0, mode );
}

int md_mkdirs( char const* dirp ) {
   return md_mkdirs2( dirp, 0, 0755 );
}

// remove a bunch of empty directories
int md_rmdirs( char const* dirp ) {
   char* dirname = strdup( dirp );
   int rc = 0;
   while( strlen(dirname) > 0 ) {
      rc = rmdir( dirname );
      if( rc != 0 ) {
         break;
      }
      else {
         char* tmp = md_dirname( dirname, NULL );
         free( dirname );
         dirname = tmp;
      }
   }
   free( dirname );
   return rc;
}

// set up a set of chubby path locks
int md_path_locks_create( struct md_path_locks* locks ) {
   locks->path_locks = new vector< long >();
   pthread_mutex_init( &locks->path_locks_lock, NULL );
   return 0;
}

// free a set of chubby path locks
int md_path_locks_free( struct md_path_locks* locks ) {
   pthread_mutex_lock( &locks->path_locks_lock );
   delete locks->path_locks;
   pthread_mutex_unlock( &locks->path_locks_lock );
   pthread_mutex_destroy( &locks->path_locks_lock );
   return 0;
}

// lock a path, spinning if needed
int md_lock_path( struct md_path_locks* locks, char const* path ) {
   long hash = md_hash( path );
   
   bool found = false;
   bool locked = false;
   
   while( 1 ) {
      if( pthread_mutex_lock( &locks->path_locks_lock ) != 0 ) {
         errorf( "[%ld] pthread_mutex_lock failed\n", pthread_self() );
         exit(1);
      }
      
      found = false;
      for( unsigned int i = 0; i < locks->path_locks->size(); i++ ) {
         if( locks->path_locks->at(i) == hash ) {
            // yup--can't lock.
            found = true;
            break;
         }
      }

      if( !found ) {
         locks->path_locks->push_back( hash );
         locked = true;
      }

      if( pthread_mutex_unlock( &locks->path_locks_lock ) != 0 ) {
         errorf( "[%ld] pthread_mutex_unlock failed\n", pthread_self() );
         exit(1);
      }

      if( locked )
         break;
   }
   return 0;
}

// lock a path in the master copy
int md_lock_mc_path( struct md_path_locks* locks, char const* mdroot, char const* path ) {
   char* fp = md_fullpath( mdroot, path, NULL );
   int rc = md_lock_path( locks, fp );
   free( fp );
   return rc;
}


// lock a path using libsyndicate's lock set
int md_global_lock_path( char const* mdroot, char const* path ) {
   return md_lock_mc_path( &md_locks, mdroot, path );
}

// unlock a path
void* md_unlock_path( struct md_path_locks* locks, char const* path ) {
   long hash = md_hash( path );
   void* ret = NULL;

   pthread_mutex_lock( &locks->path_locks_lock );
   
   for( unsigned int i = 0; i < locks->path_locks->size(); i++ ) {
      if( locks->path_locks->at(i) == hash ) {
         locks->path_locks->erase( locks->path_locks->begin() + i );
         i--;
         break;
      }
   }

   pthread_mutex_unlock( &locks->path_locks_lock );

   return ret;
}

// unlock a path in the master copy
void* md_unlock_mc_path( struct md_path_locks* locks, char const* mdroot, char const* path ) {
   char* fp = md_fullpath( mdroot, path, NULL );
   void* ret = md_unlock_path( locks, fp );
   free( fp );
   return ret;
}

// unlock a path using libsyndicate's lock set
void* md_global_unlock_path( char const* mdroot, char const* path ) {
   return md_unlock_mc_path( &md_locks, mdroot, path );
}

// start a thread
pthread_t md_start_thread( void* (*thread_func)(void*), void* arg, bool detach ) {

   // start up a thread to listen for connections
   pthread_attr_t attrs;
   pthread_t listen_thread;
   int rc;
   
   rc = pthread_attr_init( &attrs );
   if( rc != 0 ) {
      errorf( "pthread_attr_init rc = %d\n", rc);
      return -1;   // problem
   }

   if( detach ) {
      rc = pthread_attr_setdetachstate( &attrs, PTHREAD_CREATE_DETACHED );    // make a detached thread--we won't join with it
      if( rc != 0 ) {
         errorf( "pthread_attr_setdetachstate rc = %d\n", rc );
         return -1;
      }
   }
   
   rc = pthread_create( &listen_thread, &attrs, thread_func, arg );
   if( rc != 0 ) {
      errorf( "pthread_create rc = %d\n", rc );
      return -1;
   }
   
   return listen_thread;
}


// turn into a deamon
int md_daemonize( char* logfile_path, char* pidfile_path, FILE** logfile ) {

   FILE* log = NULL;
   int pid_fd = -1;
   
   if( logfile_path ) {
      log = fopen( logfile_path, "a" );
   }
   if( pidfile_path ) {
      pid_fd = open( pidfile_path, O_CREAT | O_EXCL | O_WRONLY, 0644 );
      if( pid_fd < 0 ) {
         // specified a PID file, and we couldn't make it.  someone else is running
         int errsv = -errno;
         errorf( "Failed to create PID file %s (error %d)\n", pidfile_path, errsv );
         return errsv;
      }
   }
   
   pid_t pid, sid;

   pid = fork();
   if (pid < 0) {
      int rc = -errno;
      errorf( "Failed to fork (errno = %d)\n", -errno);
      return rc;
   }

   if (pid > 0) {
      exit(0);
   }

   // child process 
   // umask(0);

   sid = setsid();
   if( sid < 0 ) {
      int rc = -errno;
      errorf("setsid errno = %d\n", rc );
      return rc;
   }

   if( chdir("/") < 0 ) {
      int rc = -errno;
      errorf("chdir errno = %d\n", rc );
      return rc;
   }

   close( STDIN_FILENO );
   close( STDOUT_FILENO );
   close( STDERR_FILENO );

   if( log ) {
      int log_fileno = fileno( log );

      if( dup2( log_fileno, STDOUT_FILENO ) < 0 ) {
         int errsv = -errno;
         errorf( "dup2 errno = %d\n", errsv);
         return errsv;
      }
      if( dup2( log_fileno, STDERR_FILENO ) < 0 ) {
         int errsv = -errno;
         errorf( "dup2 errno = %d\n", errsv);
         return errsv;
      }

      if( logfile )
         *logfile = log;
      else
         fclose( log );
   }
   else {
      int null_fileno = open("/dev/null", O_WRONLY);
      dup2( null_fileno, STDOUT_FILENO );
      dup2( null_fileno, STDERR_FILENO );
   }

   if( pid_fd >= 0 ) {
      char buf[10];
      sprintf(buf, "%d", getpid() );
      write( pid_fd, buf, strlen(buf) );
      fsync( pid_fd );
      close( pid_fd );
   }
   return 0;
}



// download data to a buffer
size_t md_default_get_callback_ram(void *stream, size_t size, size_t count, void *user_data) {
   struct md_download_buf* dlbuf = (struct md_download_buf*)user_data;
   
   size_t realsize = size * count;
   
   int new_size = realsize + dlbuf->len;
   
   char* new_buf = (char*)realloc( dlbuf->data, new_size );
   if( new_buf == NULL ) {
      free( dlbuf->data );
      dlbuf->data = NULL;
      return 0;      // out of memory
   }
   else {
      dlbuf->data = new_buf;
      memcpy( dlbuf->data + dlbuf->len, stream, realsize );
      dlbuf->len = new_size;
      return realsize;
   }
}

// download data to a response buffer
size_t md_get_callback_response_buffer( void* stream, size_t size, size_t count, void* user_data ) {
   response_buffer_t* rb = (response_buffer_t*)user_data;

   size_t realsize = size * count;
   char* buf = CALLOC_LIST( char, realsize );
   memcpy( buf, stream, realsize );
   
   rb->push_back( buffer_segment_t( buf, realsize ) );

   return realsize;
}

// download data to disk
size_t md_default_get_callback_disk(void *stream, size_t size, size_t count, void* user_data) {
   int* fd = (int*)user_data;
   
   ssize_t num_written = write( *fd, stream, size*count );
   if( num_written < 0 )
      num_written = 0;
   
   return (size_t)num_written;
}


// set the connection timeout
int md_connect_timeout( unsigned long timeout ) {
   unsigned long tmp = _connect_timeout;
   _connect_timeout = timeout;
   return tmp;
}


// no signals?
int md_signals( int use_signals ) {
   int tmp = _signals;
   _signals = use_signals;
   return tmp;
}

// download a file.  put the contents in buf
// return the size of the downloaded file (or negative on error)
ssize_t md_download_file4( char const* url, char** buf, char const* username, char const* password, char const* proxy, void (*curl_extractor)( CURL*, int, void* ), void* arg ) {
   CURL* curl_h;

   response_buffer_t rb;
   
   curl_h = curl_easy_init();
   
   curl_easy_setopt( curl_h, CURLOPT_NOPROGRESS, 1L );   // no progress bar
   curl_easy_setopt( curl_h, CURLOPT_USERAGENT, "Syndicate-agent/1.0");
   curl_easy_setopt( curl_h, CURLOPT_URL, url );
   curl_easy_setopt( curl_h, CURLOPT_FOLLOWLOCATION, 1L );
   curl_easy_setopt( curl_h, CURLOPT_FILETIME, 1L );

   if( proxy != NULL ) {
      curl_easy_setopt( curl_h, CURLOPT_PROXY, proxy );
   }
   
   char* userpass = NULL;
   if( username && password ) {
      curl_easy_setopt( curl_h, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC );
      userpass = (char*)calloc( strlen(username) + 1 + strlen(password) + 1, 1 );
      sprintf( userpass, "%s:%s", username, password );
      curl_easy_setopt( curl_h, CURLOPT_USERPWD, userpass );
   }
   if( !_signals )
      curl_easy_setopt( curl_h, CURLOPT_NOSIGNAL, 1L );
   
   curl_easy_setopt( curl_h, CURLOPT_WRITEDATA, (void*)&rb );
   curl_easy_setopt( curl_h, CURLOPT_CONNECTTIMEOUT, _connect_timeout );
   curl_easy_setopt( curl_h, CURLOPT_TIMEOUT, _transfer_timeout );
   curl_easy_setopt( curl_h, CURLOPT_WRITEFUNCTION, md_get_callback_response_buffer );
   
   int rc = curl_easy_perform( curl_h );
   if( curl_extractor != NULL ) {
      (*curl_extractor)( curl_h, rc, arg );
   }
   
   if( rc != 0 ) {
      response_buffer_free( &rb );
      
      curl_easy_cleanup( curl_h );
      
      if( userpass )
         free( userpass );
      
      return -abs(rc);
   }

   size_t len = response_buffer_size( &rb );
   *buf = response_buffer_to_string( &rb );

   response_buffer_free( &rb );
   
   curl_easy_cleanup( curl_h );
   
   if( userpass )
      free( userpass );

   return len;
}


ssize_t md_download_file2( char const* url, char** buf, char const* username, char const* password ) {
   return md_download_file4( url, buf, username, password, NULL, NULL, NULL );
}


static void status_code_extractor( CURL* curl_h, int curl_rc, void* arg ) {
   long* resp = (long*)arg;
   if( curl_rc == 0 ) {
      curl_easy_getinfo( curl_h, CURLINFO_RESPONSE_CODE, resp );
   }
}

ssize_t md_download_file( char const* url, char** buf, int* status_code ) {
   long sc = 0;
   ssize_t ret = md_download_file4( url, buf, NULL, NULL, NULL, status_code_extractor, &sc );
   if( status_code )
      *status_code = (int)sc;
   return ret;
}

ssize_t md_download_file_proxied( char const* url, char** buf, char const* proxy, int* status_code ) {
   long sc = 0;
   ssize_t ret = md_download_file4( url, buf, NULL, NULL, proxy, status_code_extractor, &sc );
   if( status_code )
      *status_code = (int)sc;
   return ret;
}


// download a file, but save it to disk
ssize_t md_download_file3( char const* url, int fd, char const* username, char const* password ) {
   
   CURL* curl_h = curl_easy_init();
   curl_easy_setopt( curl_h, CURLOPT_NOPROGRESS, 1L );   // no progress bar
   curl_easy_setopt( curl_h, CURLOPT_USERAGENT, "Syndicate-agent/1.0");
   curl_easy_setopt( curl_h, CURLOPT_URL, url );
   curl_easy_setopt( curl_h, CURLOPT_FOLLOWLOCATION, 1L );
   curl_easy_setopt( curl_h, CURLOPT_FILETIME, 1L );
   curl_easy_setopt( curl_h, CURLOPT_SSL_VERIFYPEER, CONF.verify_peer ? 1L : 0L );
   
   char* userpass = NULL;
   if( username && password ) {
      curl_easy_setopt( curl_h, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC );
      userpass = (char*)calloc( strlen(username) + 1 + strlen(password) + 1, 1 );
      sprintf( userpass, "%s:%s", username, password );
      curl_easy_setopt( curl_h, CURLOPT_USERPWD, userpass );
   }
   if( !_signals )
      curl_easy_setopt( curl_h, CURLOPT_NOSIGNAL, 1L );
   
   curl_easy_setopt( curl_h, CURLOPT_WRITEDATA, (void*)&fd );
   curl_easy_setopt( curl_h, CURLOPT_CONNECTTIMEOUT, _connect_timeout );
   curl_easy_setopt( curl_h, CURLOPT_TIMEOUT, _transfer_timeout );
   curl_easy_setopt( curl_h, CURLOPT_WRITEFUNCTION, md_default_get_callback_disk );
   
   int rc = curl_easy_perform( curl_h );
   
   curl_easy_cleanup( curl_h );
   if( userpass )
      free( userpass );
   
   return -abs(rc);
}

// download straight from an existing curl handle
ssize_t md_download_file5( CURL* curl_h, char** buf ) {
   struct md_download_buf dlbuf;
   dlbuf.len = 0;
   dlbuf.data = CALLOC_LIST( char, 1 );

   if( dlbuf.data == NULL ) {
      return -1;
   }

   curl_easy_setopt( curl_h, CURLOPT_WRITEDATA, (void*)&dlbuf );
   curl_easy_setopt( curl_h, CURLOPT_WRITEFUNCTION, md_default_get_callback_ram );
   int rc = curl_easy_perform( curl_h );

   if( rc != 0 ) {
      free( dlbuf.data );
      dlbuf.data = NULL;
      return -abs(rc);
   }

   *buf = dlbuf.data;
   return dlbuf.len;
}

// parse a query string into a list of CGI arguments
// NOTE: this modifies args_str
char** md_parse_cgi_args( char* args_str ) {
   int num_args = 1;
   for( unsigned int i = 0; i < strlen(args_str); i++ ) {
      if( args_str[i] == '&' )
         num_args++;
   }

   char** cgi_args = (char**)calloc( sizeof(char*) * (num_args+1), 1 );
   int off = 0;
   for( int i = 0; i < num_args - 1; i++ ) {
      cgi_args[i] = args_str + off;
      
      unsigned int j;
      for( j = off+1; j < strlen(args_str); j++ ) {
         if( args_str[j] == '&' )
            break;
      }
      
      args_str[j] = '\0';
      off = j+1;
   }
   cgi_args[ num_args - 1 ] = args_str + off;
   return cgi_args;
}


// degrade permissions and become a daemon
int md_release_privileges() {
   struct passwd* pwd;
   int ret = 0;
   
   // switch to "daemon" user, if possible
   pwd = getpwnam( "daemon" );
   if( pwd != NULL ) {
      setuid( pwd->pw_uid );
      dbprintf( "became user '%s'\n", "daemon" );
      ret = 0;
   }
   else {
      dbprintf( "could not become '%s'\n", "daemon" );
      ret = 1;
   }
   
   return ret;
}


// get the hostname out of a URL
char* md_url_hostname( char const* _url ) {
   char* url = strdup( _url );
   
   // find :// separator
   char* host_port = strstr( url, "://" );
   if( host_port == NULL )
      host_port = url;
   else
      host_port += 3;
   
   // consume the string to find / or :
   char* tmp = NULL;
   char* ret = strtok_r( host_port, ":/", &tmp );
   if( ret == NULL )
      // no : or /
      ret = host_port;
   
   ret = strdup( ret );
   free( url );
   return ret;
}

// locate the path from the url
char* md_path_from_url( char const* url ) {
   // find the ://, if given
   char* off = strstr( (char*)url, "://" );
   if( !off ) {
      off = (char*)url;
   }
   else {
      off += 3;         // advance to hostname
   }
   
   // find the next /
   off = strstr( off, "/" );
   char* ret = NULL;
   if( !off ) {
      // just a URL; no '/''s
      ret = strdup( "/" );
   }
   else {
      ret = strdup( off );
   }
   
   return ret;
}


// get the FS path from a URL
char* md_fs_path_from_url( char const* url ) {
   char* ret = NULL;

   // extract the prefixes
   char const* prefixes[] = {
      SYNDICATE_DATA_PREFIX,
      NULL
   };

   char const* url_path = url;

   for( int i = 0; prefixes[i] != NULL; i++ ) {
      char const* start = strstr( url_path, prefixes[i] );
      if( start != NULL ) {
         url_path = start + strlen(prefixes[i]);

         if( url_path[0] != '/' )
            url_path--;

         break;
      }
   }

   // if no prefies, then just advance to the path
   if( url_path == url ) {
      url_path = md_path_from_url( url );
   }

   ret = strdup( url_path );
   md_clear_version( ret );

   return ret;
}

// strip the path from he url 
char* md_url_strip_path( char const* url ) {
   char* ret = strdup( url );
   
   // find the ://, if given
   char* off = strstr( (char*)ret, "://" );
   if( !off ) {
      off = (char*)ret;
   }
   else {
      off += 3;         // advance to hostname
   }

   // find the next /
   off = strstr( off, "/" );
   if( off ) {
      *off = '\0';
   }

   return ret;
}

// locate the port number from the url
int md_portnum_from_url( char const* url ) {
   // find the ://, if given
   char* off = strstr( (char*)url, "://" );
   if( !off ) {
      off = (char*)url;
   }
   else {
      off += 3;         // advance to hostname
   }
   
   // find the next :
   off = strstr( off, ":" );
   if( !off ) {
      // no port number given
      return -1;
   }
   else {
      off++;
      long ret = strtol( off, NULL, 10 );
      return (int)ret;
   }
}

// strip the protocol from a url
char* md_strip_protocol( char const* url ) {
   char* off = strstr( (char*)url, "://" );
   if( !off ) {
      return strdup( url );
   }
   else {
      return strdup( off + 3 );
   }
}


// convert a URL to normal form, which the caller must free
char* md_normalize_url( char const* url, int* rc ) {
   UriParserStateA state;
   UriUriA curr;
   
   state.uri = &curr;
   if( uriParseUriA( &state, url ) != URI_SUCCESS ) {
      errorf( "could not parse %s\n", url );
      
      *rc = -EINVAL;
      uriFreeUriMembersA( &curr );
      return NULL;
   }
   
   const unsigned int dirtyParts = uriNormalizeSyntaxMaskRequiredA( &curr );
   if( uriNormalizeSyntaxExA( &curr, dirtyParts ) != URI_SUCCESS) {
      errorf( "could not normalize %s\n", url );
      
      *rc = -EINVAL;
      uriFreeUriMembersA( &curr );
      return NULL;
   }
   
   int len = 0;
   if( uriToStringCharsRequiredA( &curr, &len ) != URI_SUCCESS ) {
      errorf( "could not determine length of %s\n", url );
      
      *rc = -EINVAL;
      uriFreeUriMembersA( &curr );
      return NULL;
   }
   
   len++;
   char* ret = (char*)malloc( len * sizeof(char) );
   
   if( uriToStringA(ret, &curr, len, NULL) != URI_SUCCESS ) {
      errorf( "could not extract %s\n", url );
      
      *rc = -EINVAL;
      uriFreeUriMembersA( &curr );
      free( ret );
      return NULL;
   }
   
   uriFreeUriMembersA( &curr );

   // flatten the result
   char* scheme_offset = strstr( ret, "://" );
   if( scheme_offset != NULL && *(scheme_offset + 3) != '\0' ) {
      char* path = md_flatten_path( scheme_offset + 3 );
      char* tmp = CALLOC_LIST( char, (scheme_offset - ret) + strlen(path) + 4 );
      strncpy( tmp, ret, scheme_offset + 3 - ret );
      strcat( tmp, path );
      free( path );
      free( ret );
      ret = tmp;
   }

   return ret;
}


// normalize a list of URLs
int md_normalize_urls( char** urls, char** ret ) {

   int worst_rc = 0;
   for( int i = 0; urls[i] != NULL; i++ ) {
      int rc = 0;
      char* new_url = md_normalize_url( urls[i], &rc );
      if( rc != 0 || new_url == NULL ) {
         ret[i] = NULL;
         worst_rc = rc;
      }
      else {
         ret[i] = new_url;
      }
   }

   return worst_rc;
}


// flatten a path.  That is, remove /./, /[/]*, etc, but don't resolve ..
char* md_flatten_path( char const* path ) {
   size_t len = strlen(path);
   char* ret = CALLOC_LIST( char, len + 1 );
 
   unsigned int i = 0;
   int off = 0;
   
   while( i < len ) {
      
      // case something/[/]*/something
      if( path[i] == '/' ) {
         if( off == 0 || (off > 0 && ret[off-1] != '/') ) {
            ret[off] = path[i];
            off++;
         }
         
         i++;
         while( i < len && path[i] == '/' ) {
            i++;
         }
      }
      else if( path[i] == '.' ) {
         // case "./somethong"
         if( off == 0 && i + 1 < len && path[i+1] == '/' ) {
            i++;
         }
         // case "something/./something"
         else if( off > 0 && ret[off-1] == '/' && i + 1 < len && path[i+1] == '/' ) {
            i+=2;
         }
         // case "something/."
         else if( off > 0 && ret[off-1] == '/' && i + 1 == len ) {
            i++;
         }
         else {
            ret[off] = path[i];
            i++;
            off++;
         }
      }
      else {
         ret[off] = path[i];
         i++;
         off++;
      }
   }
   
   return ret;
}


// convert the URL into the CDN-ified form
char* md_cdn_url( char const* url ) {
   // fix the URL so it is prefixed by the hostname and CDN, instead of being file://path or http://hostname/path
   char* cdn_prefix = CONF.cdn_prefix;
   char* host_path = md_strip_protocol( url );
   if( cdn_prefix == NULL || strlen(cdn_prefix) == 0 ) {
      // no prefix given
      cdn_prefix = (char*)"http://";
   }
   char* update_url = md_fullpath( cdn_prefix, host_path, NULL );
   free( host_path );
   return update_url;
}

// publish a file.  Succeeds if the file is published.  Fails with -EEXIST if already published, or -ENOENT if there is no corresponding data, or -ENOTDIR if the data isn't in a directory
int md_publish_file( char const* data_root, char const* publish_root, char const* fs_path, int64_t file_version ) {
   char* data_path = md_publish_path_file( data_root, fs_path, file_version );
   char* publish_path = md_publish_path_file( publish_root, fs_path, file_version );

   struct stat sb;
   int rc = stat( data_path, &sb );
   if( rc != 0 ) {
      int errsv = -errno;
      free( data_path );
      free( publish_path );
      return -errsv;
   }
   
   if( !S_ISDIR( sb.st_mode ) ) {
      free( data_path );
      free( publish_path );
      return -ENOTDIR;
   }

   free( data_path );
   
   rc = md_mkdirs( publish_path );
   if( rc != 0 ) {
      errorf("md_mkdirs(%s) rc = %d\n", publish_path, rc );
      free( publish_path );
      return rc;
   }

   free( publish_path );

   return 0;
}

// publish a block.  Succeeds if the block is published.  Fails with -EEXIST if already published.  Fails with -ENOENT if the data doesn't exist
// fs_path is the path in the filesystem 
// block_id is the name of the block.  version is the version to publish
int md_publish_block( char const* data_root, char const* publish_root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version ) {
   char* publish_path = md_publish_path_block( publish_root, fs_path, file_version, block_id, block_version );

   size_t num_len = (size_t)(log( MAX((unsigned)abs(file_version), MAX(block_id, (unsigned)abs(block_version))) + 1 )) + 1;
   
   char* data_path = CALLOC_LIST( char, strlen(data_root) + 1 + strlen(fs_path) + 1 + 3 * (num_len + 1) + 1 );
   sprintf( data_path, "%s%s.%" PRId64, data_root, fs_path, file_version );

   // verify the file exists
   struct stat sb;
   int rc = stat( data_path, &sb );
   if( rc != 0 ) {
      int errsv = -errno;
      errorf("stat(%s) rc = %d\n", data_path, errsv);
      free( data_path );
      free( publish_path );
      return errsv;
   }

   // append the block ID for publishing
   sprintf( data_path, "%s%s.%" PRId64 "/%" PRIu64, data_root, fs_path, file_version, block_id );
   rc = symlink( data_path, publish_path );
   if( rc != 0 ) {
      rc = -errno;
      errorf( "symlink(%s, %s) rc = %d\n", data_path, publish_path, rc );
   }

   free( data_path );
   free( publish_path );

   return rc;
}


// perform the actual withdrawal--remove the given path if it is a symlink
// path must be an absolute path on the underlying filesystem (not an fs_path)
int md_withdraw( char const* path ) {
   struct stat statbuf;
   int rc = lstat( path, &statbuf );
   if( rc != 0 ) {
      rc = -errno;
      errorf( "Could not stat %s, errno = %d\n", path, rc );
      
      return rc;
   }
   
   if( S_ISLNK( statbuf.st_mode ) ) {
      rc = unlink( path );
      
      if( rc != 0 ) {
         errorf( "error = %d when removing %s\n", errno, path );
         rc = -errno;
      }
   }
   else {
      errorf( "%s is not a symlink; will not withdraw\n", path );
      rc = -EINVAL;
   }
   
   return rc;
}

// withdraw a file's block.
// return 0 on success.
int md_withdraw_block( char const* publish_root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version ) {
   char* path = md_publish_path_block( publish_root, fs_path, file_version, block_id, block_version );
   
   dbprintf( "Withdraw %s\n", path );
   int rc = md_withdraw( path );
   
   free( path );
   
   return rc;
}


// withdraw a filesystem entry--remove the blocks inside of its directory, and then its directory.
// The root directory can be the publish directory, staging directory, or data directory.
// TODO: ensure each unlinked item is, in fact, a block
int md_withdraw_entry( char const* root, char const* fs_path, int64_t version ) {
   int rc = 0;

   char* publish_fs_path = NULL;
   if( version >= 0 ) {
      char buf[24];
      sprintf(buf, ".%" PRId64, version );
      publish_fs_path = md_prepend( fs_path, buf, NULL );
   }
   else {
      publish_fs_path = strdup( fs_path );
   }

   char* publish_path = md_fullpath( root, publish_fs_path, NULL );
   free( publish_fs_path );
   
   DIR* dir = opendir( publish_path );
   if( dir == NULL ) {
      int errsv = -errno;
      errorf( "opendir(%s) errno = %d\n", publish_path, errsv );
      free( publish_path );
      return errsv;
   }

   int dirent_sz = offsetof(struct dirent, d_name) + pathconf(publish_path, _PC_NAME_MAX) + 1;

   struct dirent* dent = (struct dirent*)malloc( dirent_sz );;
   struct dirent* result = NULL;

   int worst_rc = 0;
   
   do {
      readdir_r( dir, dent, &result );
      if( result != NULL ) {
         if( strcmp(result->d_name, ".") == 0 ||strcmp(result->d_name, "..") == 0 )
            continue;
         
         char* fp = md_fullpath( publish_path, result->d_name, NULL );
         rc = unlink( fp );
         free( fp );

         if( rc != 0 ) {
            worst_rc = -errno;
            errorf( "unlink(%s) errno = %d\n", result->d_name, worst_rc );
         }
      }
   } while( result != NULL );

   closedir(dir);
   free( dent );
   
   rc = rmdir( publish_path );
   if( rc != 0 ) {
      rc = -errno;
      errorf( "rmdir(%s) errno = %d\n", publish_path, rc );
   }

   free( publish_path );
   
   return rc;
}

// withdraw a file
int md_withdraw_file( char const* root, char const* fs_path, int64_t version ) {
   return md_withdraw_entry( root, fs_path, version );
}

// withdraw a directory--remove it
int md_withdraw_dir( char const* root, char const* fs_path ) {
   return md_withdraw_entry( root, fs_path, -1 );
}


// given a root directory, filesystem path and the block ID, concatenate them
char* md_full_block_path( char const* root, char const* fs_path, int64_t file_version, uint64_t block_id ) {
   size_t len = strlen(root) + 1 + strlen(fs_path) + 1 + (size_t)log( (double)file_version + 1 ) + 2 + (size_t)log( (double)block_id + 1 ) + 2;
   
   char* ret = CALLOC_LIST( char, len );
   char* path = md_fullpath( root, fs_path, NULL );
   
   if( path[strlen(path)-1] == '/' )
      path[strlen(path)-1] = '\0';
   
   char buf[50];
   sprintf(buf, ".%" PRId64 "/%" PRIu64, file_version, block_id );
   strcat( ret, buf );
   
   return ret;
}


// calculate the location on disk to which this particular file would be published.
// the caller must free the returned path
// set version field to be less than 0 if you don't want the version field used
char* md_publish_path_file( char const* root, char const* fs_path, int64_t version ) {
   unsigned int ver_len = 60;
   char* ret = CALLOC_LIST( char, strlen(root) + 2 + strlen(fs_path) + 1 + ver_len + 1 );
   
   sprintf(ret, "%s/%s.%" PRId64, root, fs_path, version );
   
   return ret;
}

// calculate the location on disk to which this particular block would be published.
// the caller must free the returned path
// set version field to be less than 0 if you don't want the version field used
char* md_publish_path_block( char const* root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version ) {
   unsigned int ver_len = (unsigned int)(log(abs(block_version) + 1) + 1);
   unsigned int block_id_len = (unsigned int)(log(block_id + 1) + 1);
   unsigned int file_version_len = (unsigned int)(log(abs(file_version) + 1) + 1);
   
   char* ret = CALLOC_LIST( char, strlen(root) + 2 + strlen(fs_path) + 1 + file_version_len + 1 + block_id_len + 1 + ver_len + 1 );
   
   sprintf(ret, "%s%s.%" PRId64" /%" PRIu64" .%" PRId64, root, fs_path, file_version, block_id, block_version );
   
   return ret;
}



// calculate the location on disk to which this particular block would be held in staging.
// the caller must free the returned path
// set version field to be less than 0 if you don't want the version field used
char* md_staging_path_block( char const* staging_root, char const* fs_path, int64_t file_version, uint64_t block_id, int64_t block_version ) {
   unsigned int ver_len = (unsigned int)(log(abs(block_version) + 1) + 1);
   unsigned int block_id_len = (unsigned int)(log(block_id + 1) + 1);
   unsigned int file_version_len = (unsigned int)(log(abs(file_version) + 1) + 1);

   char* ret = CALLOC_LIST( char, strlen(staging_root) + 2 + strlen(fs_path) + 1 + file_version_len + 1 + block_id_len + 1 + ver_len + 1 );

   sprintf(ret, "%s%s.%" PRId64 "/%" PRIu64" .%" PRId64, staging_root, fs_path, file_version, block_id, block_version );

   return ret;
}


// Read the path version from a given path.
// The path version is the number attached to the end of the path by a period.
// returns a nonnegative number on success.
// returns negative on error.
int64_t md_path_version( char const* path ) {
   // find the last .
   int i;
   bool valid = true;
   for( i = strlen(path)-1; i >= 0; i-- ) {
      if( path[i] == '.' )
         break;
      
      if( path[i] < '0' || path[i] > '9' ) {
         valid = false;
         break;
      }
   }
   if( i <= 0 )
      return (int64_t)(-1);     // no version can be found
   
   if( !valid )
      return (int64_t)(-2);     // no version in the name
   
   char *end;
   int64_t version = strtoll( path + i + 1, &end, 10 );
   if( version == 0 && *end != '\0' )
      return (int64_t)(-3);     // could not parse the version

   return version;
}


// Get the offset into a path where the version begins.
// Returns nonnegative on success.
// returns negative on error.
int md_path_version_offset( char const* path ) {
   int i;
   bool valid = true;
   for( i = strlen(path)-1; i >= 0; i-- ) {
      if( path[i] == '.' )
         break;
      if( path[i] < '0' || path[i] > '9' ) {
         valid = false;
         break;
      }
   }
   
   if( !valid ) {
      return -2;
   }
   
   if( i <= 0 ) {
      return -1;
   }
   
   char *end;
   long version = strtol( path + i + 1, &end, 10 );
   if( version == 0 && *end != '\0' )
      return -3;     // could not parse the version
   
   return i;  
}


// given two paths, determine if one is the versioned form of the other
bool md_is_versioned_form( char const* vanilla_path, char const* versioned_path ) {
   // if this isn't a versioned path, then no
   int version_offset = md_path_version_offset( versioned_path );
   if( version_offset <= 0 )
      return false;
   
   if( strlen(vanilla_path) > (unsigned)version_offset )
      return false;
   
   if( strncmp( vanilla_path, versioned_path, version_offset ) != 0 )
      return false;
   
   return true;
}

// find versioned copies of a given file.  base_path must be a full path
char** md_versioned_paths( char const* base_path ) {
   char* base_path_dir = md_dirname( base_path, NULL );
   char* base_path_basename = md_basename( base_path, NULL );
   
   DIR* dir = opendir( base_path_dir );
   if( dir == NULL ) {
      int errsv = errno;
      errorf( "could not open %s, errno = %d\n", base_path_dir, errsv );
      free( base_path_dir );
      free( base_path_basename );
      return NULL;
   }
   
   int dirent_sz = offsetof(struct dirent, d_name) + pathconf(base_path_dir, _PC_NAME_MAX) + 1;
   
   struct dirent* dent = (struct dirent*)malloc( dirent_sz );;
   struct dirent* result = NULL;
   
   vector<char*> copies;
   
   do {
      readdir_r( dir, dent, &result );
      if( result != NULL ) {
         if( md_is_versioned_form( base_path_basename, result->d_name ) ) {
            copies.push_back( md_fullpath( base_path_dir, result->d_name, NULL ) );
         }
      }
   } while( result != NULL );
   
   char** ret = (char**)malloc( sizeof(char*) * (copies.size() + 1) );
   for( vector<char*>::size_type i = 0; i < copies.size(); i++ ) {
      ret[i] = copies[i];
   }
   ret[copies.size()] = NULL;
   
   closedir(dir);
   free( dent );
   free( base_path_dir );
   free( base_path_basename );
   
   return ret;
}


// find the versions of an unversioned path.
// returned list is terminated by a -1
int64_t* md_versions( char const* base_path ) {
   char* base_path_dir = md_dirname( base_path, NULL );
   char* base_path_basename = md_basename( base_path, NULL );

   DIR* dir = opendir( base_path_dir );
   if( dir == NULL ) {
      int errsv = errno;
      errorf( "could not open %s, errno = %d\n", base_path_dir, errsv );
      return NULL;
   }

   int dirent_sz = offsetof(struct dirent, d_name) + pathconf(base_path_dir, _PC_NAME_MAX) + 1;

   struct dirent* dent = (struct dirent*)malloc( dirent_sz );;
   struct dirent* result = NULL;

   vector<int64_t> versions;

   do {
      readdir_r( dir, dent, &result );
      if( result != NULL ) {
         if( md_is_versioned_form( base_path_basename, result->d_name ) ) {
            int64_t ver = md_path_version( result->d_name );
            if( ver >= 0 )
               versions.push_back( ver );
         }
      }
   } while( result != NULL );

   int64_t* ret = CALLOC_LIST( int64_t, versions.size() + 1 );
   for( unsigned int i = 0; i < versions.size(); i++ ) {
      ret[i] = versions[i];
   }
   ret[ versions.size() ] = (int64_t)(-1);

   closedir(dir);
   free( dent );
   free( base_path_dir );
   free( base_path_basename );

   return ret;
}




// clear the version of a path
char* md_clear_version( char* path ) {
   int off = md_path_version_offset( path );
   if( off > 0 ) {
      path[off] = '\0';
   }
   return path;
}

// find the next version of a given set of paths
int64_t md_next_version( char** versioned_publish_paths ) {
   int64_t next_version = 1;
   if( versioned_publish_paths[0] != NULL ) {
      // this path is meant to be published for the first time.
      // find the highest version number
      next_version = -1;
      for( int i = 0; versioned_publish_paths[i] != NULL; i++ ) {
         int64_t ver = md_path_version( versioned_publish_paths[i] );
         if( ver > next_version )
            next_version = ver;
      }
      next_version++;
   }
   return next_version;
}

// find the next version from a set of versions
int64_t md_next_version( int64_t* versions ) {
   int64_t next_version = -1;
   if( versions != NULL ) {
      for( int i = 0; versions[i] >= 0; i++ ) {
         next_version = MAX( next_version, versions[i] );
      }
   }
   return next_version + 1;
}


// serialize updates

// iterator for lists
struct md_metadata_update_list_data {
   uint64_t next;
   struct md_update** updates;
};

static struct md_update* md_metadata_update_list_iterator( void* arg ) {
   struct md_metadata_update_list_data* itrdata = (struct md_metadata_update_list_data*)arg;

   struct md_update* ret = itrdata->updates[ itrdata->next ];
   itrdata->next++;
   return ret;
}

ssize_t md_metadata_update_text( struct md_syndicate_conf* conf, char** buf, struct md_update** updates ) {
   struct md_metadata_update_list_data itrdata;
   itrdata.updates = updates;
   itrdata.next = 0;

   return md_metadata_update_text3( conf, buf, md_metadata_update_list_iterator, &itrdata );
}

// iterator for vectors
struct md_metadata_update_vector_iterator_data {
   uint64_t next;
   vector<struct md_update>* updates;
};

static struct md_update* md_metadata_update_vector_iterator( void* arg ) {
   struct md_metadata_update_vector_iterator_data* itrdata = (struct md_metadata_update_vector_iterator_data*)arg;

   if( itrdata->next >= itrdata->updates->size() )
      return NULL;

   struct md_update* up = &itrdata->updates->at( itrdata->next );
   itrdata->next++;
   return up;
}

ssize_t md_metadata_update_text2( struct md_syndicate_conf* conf, char** buf, vector<struct md_update>* updates ) {
   struct md_metadata_update_vector_iterator_data itrdata;
   itrdata.updates = updates;
   itrdata.next = 0;

   return md_metadata_update_text3( conf, buf, md_metadata_update_vector_iterator, &itrdata );
}

// convert an md_entry to an ms_entry
int md_entry_to_ms_entry( struct md_syndicate_conf* conf, ms::ms_entry* msent, struct md_entry* ent ) {

   if( ent->url == NULL ) {
      errorf("%s", "ent->url == NULL\n");
      return -EINVAL;
   }

   if( ent->path == NULL ) {
      errorf("%s", "ent->url == NULL\n");
      return -EINVAL;
   }
   
   msent->set_url( string(ent->url) );
   
   char* path_sane = strdup(ent->path);
   md_sanitize_path( path_sane );

   msent->set_type( ent->type == MD_ENTRY_FILE ? ms::ms_entry::MS_ENTRY_TYPE_FILE : ms::ms_entry::MS_ENTRY_TYPE_DIR );
   msent->set_path( string(path_sane) );
   msent->set_owner( ent->owner );
   msent->set_volume( ent->volume );
   msent->set_mode( ent->mode );
   msent->set_ctime_sec( ent->ctime_sec );
   msent->set_ctime_nsec( ent->ctime_nsec );
   msent->set_mtime_sec( ent->mtime_sec );
   msent->set_mtime_nsec( ent->mtime_nsec );
   msent->set_version( ent->version );
   msent->set_size( ent->size );
   msent->set_max_read_freshness( ent->max_read_freshness );
   msent->set_max_write_freshness( ent->max_write_freshness );

   free( path_sane );
   
   return 0;
}


// convert ms_entry to md_entry
int ms_entry_to_md_entry( struct md_syndicate_conf* conf, const ms::ms_entry& msent, struct md_entry* ent ) {
   memset( ent, 0, sizeof(struct md_entry) );

   ent->url = strdup( msent.url().c_str() );
   ent->type = msent.type() == ms::ms_entry::MS_ENTRY_TYPE_FILE ? MD_ENTRY_FILE : MD_ENTRY_DIR;

   ent->path = strdup( msent.path().c_str() );
   md_sanitize_path( ent->path );
   
   ent->owner = msent.owner();
   ent->volume = msent.volume();
   ent->mode = msent.mode();
   ent->mtime_sec = msent.mtime_sec();
   ent->mtime_nsec = msent.mtime_nsec();
   ent->ctime_sec = msent.ctime_sec();
   ent->ctime_nsec = msent.ctime_nsec();
   ent->max_read_freshness = (uint64_t)msent.max_read_freshness();
   ent->max_write_freshness = (uint64_t)msent.max_write_freshness();
   ent->version = msent.version();
   ent->size = msent.size();
   
   return 0;
}



// iterate through a set of updates and serialize them
ssize_t md_metadata_update_text3( struct md_syndicate_conf* conf, char** buf, struct md_update* (*iterator)( void* ), void* arg ) {

   ms::ms_updates updates;

   while( 1 ) {
      struct md_update* update = (*iterator)( arg );
      if( update == NULL )
         break;

      ms::ms_update* ms_up = updates.add_updates();

      ms_up->set_type( update->op );

      ms::ms_entry* ms_ent = ms_up->mutable_entry();

      md_entry_to_ms_entry( conf, ms_ent, &update->ent );
   }

   string text;
   
   bool valid = updates.SerializeToString( &text );
   if( !valid ) {
      return -1;
   }

   *buf = CALLOC_LIST( char, text.size() + 1 );
   memcpy( *buf, text.data(), text.size() );

   return (ssize_t)text.size();
}

// default callback function to be used when posting to a server
size_t md_default_post_callback(void *ptr, size_t size, size_t nmemb, void *userp) {
   size_t realsize = size * nmemb;
   struct md_post_buf* post = (struct md_post_buf*)userp;
   size_t writesize = min( post->len - post->offset, (int)realsize );
   
   if( post->offset >= post->len )
      return 0;
   
   memcpy( ptr, post->text + post->offset, writesize );
   post->offset += realsize;
   
   if( post->offset >= post->len ) {
      return writesize;
   }
   else {
      return realsize;
   }  
}


// free a metadata update
void md_update_free( struct md_update* update ) {
   md_entry_free( &update->ent );
}


// duplicate an update
void md_update_dup2( struct md_update* src, struct md_update* dest ) {
   dest->op = src->op;
   dest->timestamp = src->timestamp;
   md_entry_dup2( &src->ent, &dest->ent );
}


// respond to a request
static int md_HTTP_default_send_response( struct MHD_Connection* connection, int status_code, char* data ) {
   char const* page = NULL;
   struct MHD_Response* response = NULL;
   
   if( data == NULL ) {
      // use a built-in status message
      switch( status_code ) {
         case MHD_HTTP_BAD_REQUEST:
            page = MD_HTTP_400_MSG;
            break;
         
         case MHD_HTTP_INTERNAL_SERVER_ERROR:
            page = MD_HTTP_500_MSG;
            break;
         
         case MHD_HTTP_UNAUTHORIZED:
            page = MD_HTTP_401_MSG;
            break;
            
         default:
            page = MD_HTTP_DEFAULT_MSG;
            break;
      }
      response = MHD_create_response_from_buffer( strlen(page), (void*)page, MHD_RESPMEM_PERSISTENT );
   }
   else {
      // use the given status message
      response = MHD_create_response_from_buffer( strlen(data), (void*)data, MHD_RESPMEM_MUST_FREE );
   }
   
   if( !response )
      return MHD_NO;
      
   // this is always a text/plain type
   MHD_add_response_header( response, "Content-Type", "text/plain" );
   int ret = MHD_queue_response( connection, status_code, response );
   MHD_destroy_response( response );
   
   return ret;
}

// make a RAM response which we'll defensively copy
int md_create_HTTP_response_ram( struct md_HTTP_response* resp, char const* mimetype, int status, char const* data, int len ) {
   resp->resp = MHD_create_response_from_buffer( len, (void*)data, MHD_RESPMEM_MUST_COPY );
   resp->status = status;
   MHD_add_response_header( resp->resp, "Content-Type", mimetype );
   return 0;
}

// make a RAM response which we'll keep a pointer to and free later
int md_create_HTTP_response_ram_nocopy( struct md_HTTP_response* resp, char const* mimetype, int status, char const* data, int len ) {
   resp->resp = MHD_create_response_from_buffer( len, (void*)data, MHD_RESPMEM_MUST_FREE );
   resp->status = status;
   MHD_add_response_header( resp->resp, "Content-Type", mimetype );
   return 0;
}

// make a RAM response which is persistent
int md_create_HTTP_response_ram_static( struct md_HTTP_response* resp, char const* mimetype, int status, char const* data, int len ) {
   resp->resp = MHD_create_response_from_buffer( len, (void*)data, MHD_RESPMEM_PERSISTENT );
   resp->status = status;
   MHD_add_response_header( resp->resp, "Content-Type", mimetype );
   return 0;
}

// make an FD-based response
int md_create_HTTP_response_fd( struct md_HTTP_response* resp, char const* mimetype, int status, int fd, off_t offset, size_t size ) {
   resp->resp = MHD_create_response_from_fd_at_offset( size, fd, offset );
   resp->status = status;
   MHD_add_response_header( resp->resp, "Content-Type", mimetype );
   return 0;
}

// make a callback response
int md_create_HTTP_response_stream( struct md_HTTP_response* resp, char const* mimetype, int status, uint64_t size, size_t blk_size, md_HTTP_stream_callback scb, void* cls, md_HTTP_free_cls_callback fcb ) {
   resp->resp = MHD_create_response_from_callback( size, blk_size, scb, cls, fcb );
   resp->status = status;
   MHD_add_response_header( resp->resp, "Content-Type", mimetype );
   return 0;
}

// give back a user-callback-created response
static int md_HTTP_send_response( struct MHD_Connection* connection, struct md_HTTP_response* resp ) {
   int ret = MHD_queue_response( connection, resp->status, resp->resp );
   MHD_destroy_response( resp->resp );
   
   return ret;
}


// free an HTTP response
void md_free_HTTP_response( struct md_HTTP_response* resp ) {
   return;
}


// find an http header value
char const* md_find_HTTP_header( struct md_HTTP_header** headers, char const* header ) {
   for( int i = 0; headers[i] != NULL; i++ ) {
      if( strcasecmp( headers[i]->header, header ) == 0 ) {
         return headers[i]->value;
      }
   }
   return NULL;
}


// find a user entry, given a username
struct md_user_entry* md_find_user_entry( char const* username, struct md_user_entry** users ) {
   for( int i = 0; users[i] != NULL; i++ ) {
      if( strcmp( users[i]->username, username ) == 0 )
         return users[i];
   }
   return NULL;
}


// find a user entry, given a uid
struct md_user_entry* md_find_user_entry2( uid_t uid, struct md_user_entry** users ) {
   for( int i = 0; users[i] != NULL; i++ ) {
      if( users[i]->uid == uid )
         return users[i];
   }
   return NULL;
}

// validate a user entry, given the password
bool md_validate_user_password( char const* password, struct md_user_entry* uent ) {
   // hash the password
   unsigned char* password_sha256 = sha256_hash( password );
   char* password_hash = sha256_printable( password_sha256 );
   free( password_sha256 );
   
   bool rc = (strcasecmp( password_hash, uent->password_hash ) == 0 );
   
   free( password_hash );
   return rc;
}


// add a header
int md_HTTP_add_header( struct md_HTTP_response* resp, char const* header, char const* value ) {
   if( resp->resp != NULL ) {
      MHD_add_response_header( resp->resp, header, value );
   }
   return 0;
}


// get the user's data out of a syndicate-managed connection data structure
void* md_cls_get( void* cls ) {
   struct md_HTTP_connection_data* dat = (struct md_HTTP_connection_data*)cls;
   return dat->cls;
}

// set the status of a syndicate-managed connection data structure
void md_cls_set_status( void* cls, int status ) {
   struct md_HTTP_connection_data* dat = (struct md_HTTP_connection_data*)cls;
   dat->status = status;
}

// set the response of a syndicate-managed connection data structure
struct md_HTTP_response* md_cls_set_response( void* cls, struct md_HTTP_response* resp ) {
   struct md_HTTP_connection_data* dat = (struct md_HTTP_connection_data*)cls;
   struct md_HTTP_response* ret = dat->resp;
   dat->resp = resp;
   return ret;
}


// create an http header
void md_create_HTTP_header( struct md_HTTP_header* header, char const* h, char const* v ) {
   header->header = strdup( h );
   header->value = strdup( v );
}

// free an HTTP header
void md_free_HTTP_header( struct md_HTTP_header* header ) {
   if( header->header ) {
      free( header->header );
   }
   if( header->value ) {
      free( header->value );
   }
   memset( header, 0, sizeof(struct md_HTTP_header) );
}

// free a download buffer
void md_free_download_buf( struct md_download_buf* buf ) {
   if( buf->data ) {
      free( buf->data );
      buf->data = NULL;
   }
   buf->len = 0;
}


// initialze a curl handle
void md_init_curl_handle( CURL* curl_h, char const* url, time_t query_timeout ) {
   curl_easy_setopt( curl_h, CURLOPT_NOPROGRESS, 1L );   // no progress bar
   curl_easy_setopt( curl_h, CURLOPT_USERAGENT, "Syndicate-agent/1.0");
   curl_easy_setopt( curl_h, CURLOPT_URL, url );
   curl_easy_setopt( curl_h, CURLOPT_FOLLOWLOCATION, 1L );
   curl_easy_setopt( curl_h, CURLOPT_NOSIGNAL, 1L );
   curl_easy_setopt( curl_h, CURLOPT_CONNECTTIMEOUT, query_timeout );
   curl_easy_setopt( curl_h, CURLOPT_FILETIME, 1L );
   //curl_easy_setopt( curl_h, CURLOPT_VERBOSE, 1L );
}


// accumulate inbound headers
static int md_accumulate_headers( void* cls, enum MHD_ValueKind kind, char const* key, char const* value ) {
   vector<struct md_HTTP_header*> *header_list = (vector<struct md_HTTP_header*> *)cls;
   
   struct md_HTTP_header* hdr = (struct md_HTTP_header*)calloc( sizeof(struct md_HTTP_header), 1 );
   md_create_HTTP_header( hdr, key, value );
   
   header_list->push_back( hdr );
   return MHD_YES;
}

// free a list of headers
static void md_free_headers( struct md_HTTP_header** headers ) {
   for( unsigned int i = 0; headers[i] != NULL; i++ ) {
      md_free_HTTP_header( headers[i] );
      free( headers[i] );
   }
   free( headers );
}

// short message upload handler, for accumulating smaller messages into RAM via a response buffer
int md_response_buffer_upload_iterator(void *coninfo_cls, enum MHD_ValueKind kind,
                                       const char *key,
                                       const char *filename, const char *content_type,
                                       const char *transfer_encoding, const char *data,
                                       uint64_t off, size_t size) {


   dbprintf( "upload %zu bytes\n", size );

   struct md_HTTP_connection_data *md_con_data = (struct md_HTTP_connection_data*)coninfo_cls;
   response_buffer_t* rb = md_con_data->rb;

   // add a copy of the data
   char* data_dup = CALLOC_LIST( char, size );
   memcpy( data_dup, data, size );

   rb->push_back( buffer_segment_t( data_dup, size ) );

   return MHD_YES;
}



// HTTP connection handler
static int md_HTTP_connection_handler( void* cls, struct MHD_Connection* connection, 
                                       char const* url, 
                                       char const* method, 
                                       char const* version, 
                                       char const* upload_data, size_t* upload_size, 
                                       void** con_cls ) {
   
   struct md_HTTP* http_ctx = (struct md_HTTP*)cls;

   md_http_rlock( http_ctx );
   
   struct md_user_entry** users = http_ctx->users;

   md_http_unlock( http_ctx );
   
   // need to create connection data?
   if( *con_cls == NULL ) {

      md_http_rlock( http_ctx );

      users = http_ctx->users;

      // verify that the URL starts with '/'
      if( strlen(url) > 0 && url[0] != '/' ) {
         errorf( "malformed URL %s\n", url );
         md_http_unlock( http_ctx );
         return md_HTTP_default_send_response( connection, MHD_HTTP_BAD_REQUEST, NULL );
      }
      
      struct md_HTTP_connection_data* con_data = CALLOC_LIST( struct md_HTTP_connection_data, 1 );
      if( !con_data ) {
         md_http_unlock( http_ctx );
         return MHD_NO;
      }
         
      struct md_user_entry* uent = NULL;
      if( users ) {
         // authenticate the user, if users are given
         char* password = NULL;
         char* username = MHD_basic_auth_get_username_password( connection, &password );
         
         char* password_ptr = password;
         if( password_ptr == NULL )
            password_ptr = (char*)"";
         
         if( username ) {
            uent = md_find_user_entry( username, users );
            if( uent == NULL ) {
               // user does not exist
               dbprintf( "user '%s' not found\n", username);
               free( username );
               free( con_data );
               md_http_unlock( http_ctx );
               return md_HTTP_default_send_response( connection, MHD_HTTP_UNAUTHORIZED, NULL );
            }
            if( !md_validate_user_password( password_ptr, uent ) ) {
               // invalid password
               dbprintf( "invalid password for user '%s'\n", username);
               free( username );
               free( con_data );
               md_http_unlock( http_ctx );
               return md_HTTP_default_send_response( connection, MHD_HTTP_UNAUTHORIZED, NULL );
            }
            
            free( username );
         }
         
         if( password )
            free( password );
      }
      else {
         // TODO: webdav
         // make sure we don't need authentication
         if( strcmp( method, "GET") == 0 && (http_ctx->authentication_mode & HTTP_AUTHENTICATE_READ) ) {
            // authentication is needed
            errorf( "%s", "no username given, but we require authentication for GET (READ)\n");
            md_http_unlock( http_ctx );
            return md_HTTP_default_send_response( connection, MHD_HTTP_UNAUTHORIZED, NULL );
         }
         else if( (strcmp( method, "PUT" ) == 0 || strcmp( method, "POST" ) == 0) && (http_ctx->authentication_mode & HTTP_AUTHENTICATE_WRITE) ) {
            // authentication is needed
            errorf( "%s", "no username given, but we require authentication for POST (WRITE)\n");
            md_http_unlock( http_ctx );
            return md_HTTP_default_send_response( connection, MHD_HTTP_UNAUTHORIZED, NULL );
         }
      }
      
      int mode = MD_HTTP_UNKNOWN;
      struct MHD_PostProcessor* pp = NULL;
      
      if( strcmp( method, "GET" ) == 0 ) {
         if( http_ctx->HTTP_GET_handler )
            mode = MD_HTTP_GET;
      }
      else if( strcmp( method, "HEAD" ) == 0 ) {
         if( http_ctx->HTTP_HEAD_handler )
            mode = MD_HTTP_HEAD;
      }
      else if( strcmp( method, "POST" ) == 0 ) {
         
         if( http_ctx->HTTP_POST_iterator ) {

            char const* encoding = MHD_lookup_connection_value( connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE );
            if( encoding != NULL &&
               (strncasecmp( encoding, MHD_HTTP_POST_ENCODING_FORM_URLENCODED, strlen( MHD_HTTP_POST_ENCODING_FORM_URLENCODED ) ) == 0 ||
                strncasecmp( encoding, MHD_HTTP_POST_ENCODING_MULTIPART_FORMDATA, strlen( MHD_HTTP_POST_ENCODING_MULTIPART_FORMDATA ) ) == 0 )) {

               pp = MHD_create_post_processor( connection, 4096, http_ctx->HTTP_POST_iterator, con_data );
               if( pp == NULL ) {
                  errorf( "failed to create POST processor for %s\n", method);
                  free( con_data );
                  md_http_unlock( http_ctx );
                  return md_HTTP_default_send_response( connection, 400, NULL );
               }
            }
            else {
               con_data->offset = 0;
            }

            mode = MD_HTTP_POST;
         }
      }
      else if( strcmp( method, "PUT" ) == 0 ) {

         if( http_ctx->HTTP_PUT_iterator ) {
            
            char const* encoding = MHD_lookup_connection_value( connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE );
            if( strncasecmp( encoding, MHD_HTTP_POST_ENCODING_FORM_URLENCODED, strlen( MHD_HTTP_POST_ENCODING_FORM_URLENCODED ) ) == 0 ||
                strncasecmp( encoding, MHD_HTTP_POST_ENCODING_MULTIPART_FORMDATA, strlen( MHD_HTTP_POST_ENCODING_MULTIPART_FORMDATA ) ) == 0 ) {

               pp = MHD_create_post_processor( connection, 4096, http_ctx->HTTP_PUT_iterator, con_data );
               if( pp == NULL ) {
                  errorf( "failed to create POST processor for %s\n", method);
                  free( con_data );
                  md_http_unlock( http_ctx );
                  return md_HTTP_default_send_response( connection, 400, NULL );
               }
            }
            else {
               con_data->offset = 0;
            }
            mode = MD_HTTP_PUT;
         }
      }
      else if( strcmp( method, "DELETE" ) == 0 ) {
         if( http_ctx->HTTP_DELETE_handler )
            mode = MD_HTTP_DELETE;
      }

      if( mode == MD_HTTP_UNKNOWN ) {
         // unsupported method
         struct md_HTTP_response* resp = CALLOC_LIST(struct md_HTTP_response, 1);
         md_create_HTTP_response_ram_static( resp, "text/plain", 501, MD_HTTP_501_MSG, strlen(MD_HTTP_501_MSG) + 1 );
         md_http_unlock( http_ctx );
         return md_HTTP_send_response( connection, resp );
      }
      
      if( uent == NULL ) {
         // make a "guest" user 
         uent = &md_guest_user;
      }
      
      con_data->user = md_user_entry_dup( uent );
      con_data->pp = pp;
      con_data->status = 200;
      con_data->mode = mode;
      con_data->conf = http_ctx->conf;
      con_data->http = http_ctx;
      con_data->url_path = md_flatten_path( url );
      con_data->version = strdup(version);
      con_data->query_string = (char*)index( con_data->url_path, '?' );
      con_data->rb = new response_buffer_t();
      con_data->remote_host = MHD_lookup_connection_value( connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_HOST );
      con_data->method = method;

      // done reading this 
      md_http_unlock( http_ctx );

      char const* content_length_str = MHD_lookup_connection_value( connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_LENGTH );
      if( content_length_str != NULL ) 
         con_data->content_length = strtol( content_length_str, NULL, 10 );
      else
         con_data->content_length = 0;
      
      if( index( con_data->url_path, '?' ) != NULL ) {
         char* p = (char*)index( con_data->url_path, '?' );
         *p = 0;
         con_data->url_path = p;
      }
      
      // get headers
      vector<struct md_HTTP_header*> headers_vec;
      MHD_get_connection_values( connection, MHD_HEADER_KIND, md_accumulate_headers, (void*)&headers_vec );

      // convert to list
      struct md_HTTP_header** headers = CALLOC_LIST( struct md_HTTP_header*, headers_vec.size() + 1 );
      for( unsigned int i = 0; i < headers_vec.size(); i++ ) {
         headers[i] = headers_vec.at(i);
      }

      con_data->headers = headers;

      con_data->cls = NULL;
      *con_cls = con_data;
      
      if( http_ctx->HTTP_connect ) {
         con_data->cls = (*http_ctx->HTTP_connect)( con_data );

         if( con_data->status != 200 ) {
            // error
            return md_HTTP_send_response( connection, con_data->resp );
         }
      }

      
      return MHD_YES;
   }

   struct md_HTTP_connection_data* con_data = (struct md_HTTP_connection_data*)(*con_cls);
   // writers

   // POST
   if( con_data->mode == MD_HTTP_POST ) {

      if( *upload_size != 0 && con_data->status > 0 ) {
         if( con_data->pp ) {
            dbprintf( "POST %s, postprocess %zu bytes\n", con_data->url_path, *upload_size );
            MHD_post_process( con_data->pp, upload_data, *upload_size );
            *upload_size = 0;
            return MHD_YES;
         }
         else {
            dbprintf( "POST %s, raw %zu bytes\n", con_data->url_path, *upload_size );
            int rc = (*http_ctx->HTTP_POST_iterator)( con_data, MHD_POSTDATA_KIND, NULL, NULL, NULL, NULL, upload_data, con_data->offset, *upload_size );
            con_data->offset += *upload_size;
            *upload_size = 0;
            return rc;
         }
      }
      else {
         (*http_ctx->HTTP_POST_finish)( con_data );
         dbprintf( "POST finished (%s)\n", url);
         
         if( con_data->resp == NULL ) {
            return md_HTTP_default_send_response( connection, 500, NULL );
         }
         else {
            return md_HTTP_send_response( connection, con_data->resp );
         }
      }
   }
   
   // PUT
   else if( con_data->mode == MD_HTTP_PUT ) {

      if( *upload_size != 0 && con_data->status > 0 ) {
         if( con_data->pp ) {
            dbprintf( "PUT %s, postprocess %zu bytes\n", con_data->url_path, *upload_size );
            MHD_post_process( con_data->pp, upload_data, *upload_size );
            *upload_size = 0;
            return MHD_YES;
         }
         else {
            dbprintf( "PUT %s, raw %zu bytes\n", con_data->url_path, *upload_size );
            int rc = (*http_ctx->HTTP_PUT_iterator)( con_data, MHD_POSTDATA_KIND, NULL, NULL, NULL, NULL, upload_data, con_data->offset, *upload_size );
            con_data->offset += *upload_size;
            *upload_size = 0;
            return rc;
         }
      }
      else {
         
         (*http_ctx->HTTP_PUT_finish)( con_data );
         dbprintf( "PUT finished (%s)\n", url);
         
         if( con_data->resp == NULL ) {
            return md_HTTP_default_send_response( connection, 500, NULL );
         }
         else {
            return md_HTTP_send_response( connection, con_data->resp );
         }
      }
   }

   // DELETE
   else if( con_data->mode == MD_HTTP_DELETE ) {
      dbprintf( "DELETE %s\n", con_data->url_path );

      struct md_HTTP_response* resp = (*http_ctx->HTTP_DELETE_handler)( con_data, 0 );

      if( resp == NULL ) {
         resp = CALLOC_LIST(struct md_HTTP_response, 1);
         md_create_HTTP_response_ram_static( resp, (char*)"text/plain", 500, MD_HTTP_500_MSG, strlen(MD_HTTP_500_MSG) + 1 );
      }

      con_data->resp = resp;
      return md_HTTP_send_response( connection, con_data->resp );
   }
   
   // GET
   else if( con_data->mode == MD_HTTP_GET ) {
      
      dbprintf( "GET %s\n", con_data->url_path);

      struct md_HTTP_response* resp = (*http_ctx->HTTP_GET_handler)( con_data );
      if( resp == NULL ) {
         resp = CALLOC_LIST(struct md_HTTP_response, 1);
         md_create_HTTP_response_ram_static( resp, (char*)"text/plain", 500, MD_HTTP_500_MSG, strlen(MD_HTTP_500_MSG) + 1 );
      }

      con_data->resp = resp;

      return md_HTTP_send_response( connection, resp );
   }
   
   // HEAD
   else if( con_data->mode == MD_HTTP_HEAD ) {
      
      dbprintf( "HEAD %s\n", con_data->url_path );

      struct md_HTTP_response* resp = (*http_ctx->HTTP_HEAD_handler)( con_data );
      if( resp == NULL ) {
         resp = CALLOC_LIST(struct md_HTTP_response, 1);
         md_create_HTTP_response_ram_static( resp, (char*)"text/plain", 500, MD_HTTP_500_MSG, strlen(MD_HTTP_500_MSG) + 1 );
      }

      con_data->resp = resp;

      return md_HTTP_send_response( connection, resp );
   }

   return md_HTTP_default_send_response( connection, MHD_HTTP_BAD_REQUEST, NULL );
}

// default cleanup handler
// calls user-supplied cleanup handler as well
void md_HTTP_cleanup( void *cls, struct MHD_Connection *connection, void **con_cls, enum MHD_RequestTerminationCode term ) {
   struct md_HTTP* http = (struct md_HTTP*)cls;
   
   struct md_HTTP_connection_data* con_data = NULL;
   if( con_cls ) {
      con_data = (struct md_HTTP_connection_data*)(*con_cls);
   }
   
   if( http->HTTP_cleanup && con_data) {
      (*http->HTTP_cleanup)( connection, con_data->cls, term );
      con_data->cls = NULL;
   }
   if( con_data ) {
      if( con_data->pp ) {
         MHD_destroy_post_processor( con_data->pp );
      }
      if( con_data->resp ) {
         md_free_HTTP_response( con_data->resp );
         free( con_data->resp );
      }
      if( con_data->url_path ) {
         free( con_data->url_path );
         con_data->url_path = NULL;
      }
      if( con_data->query_string ) {
         free( con_data->query_string );
         con_data->query_string = NULL;
      }
      if( con_data->version ) {
         free( con_data->version );
         con_data->version = NULL;
      }
      if( con_data->headers ) {
         md_free_headers( con_data->headers );
         con_data->headers = NULL;
      }
      if( con_data->rb ) {
         response_buffer_free( con_data->rb );
         delete con_data->rb;
         con_data->rb = NULL;
      }
      if( con_data->user ) {
         md_free_user_entry( con_data->user );
         free( con_data->user );
         con_data->user = NULL;
      }
      
      free( con_data );
   }
}

// set fields in an HTTP structure
int md_HTTP_init( struct md_HTTP* http, int server_type, struct md_syndicate_conf* conf, struct md_user_entry** users ) {
   memset( http, 0, sizeof(struct md_HTTP) );
   http->conf = conf;
   http->users = users;
   http->server_type = server_type;
   return 0;
}


// start the HTTP thread
int md_start_HTTP( struct md_HTTP* http, int portnum ) {
   char* server_key = NULL;
   char* server_cert = NULL;
   
   size_t server_key_len = 0;
   size_t server_cert_len = 0;
   struct md_syndicate_conf* conf = http->conf;
   
   if( conf->server_key && conf->server_cert ) {
      server_key = load_file( conf->server_key, &server_key_len );
      server_cert = load_file( conf->server_cert, &server_cert_len );
      
      if( !server_key ) {
         errorf( "Could not read server private key %s\n", conf->server_key );
      }
      if( !server_cert ) {
         errorf( "Could not read server certificate %s\n", conf->server_cert );
      }

      server_key = (char*)realloc( server_key, server_key_len+1 );
      server_cert = (char*)realloc( server_cert, server_cert_len+1 );

      server_key[server_key_len] = 0;
      server_cert[server_cert_len] = 0;
   }

   pthread_rwlock_init( &http->lock, NULL );
   
   if( server_cert && server_key ) {

      http->server_pkey = server_key;
      http->server_cert = server_cert;
      
      // SSL enabled
      if( http->server_type == MHD_USE_THREAD_PER_CONNECTION ) {
         http->http_daemon = MHD_start_daemon(  http->server_type | MHD_USE_SSL, portnum, NULL, NULL, &md_HTTP_connection_handler, http,
                                                MHD_OPTION_HTTPS_MEM_KEY, server_key, 
                                                MHD_OPTION_HTTPS_MEM_CERT, server_cert,
                                                MHD_OPTION_NOTIFY_COMPLETED, md_HTTP_cleanup, http,
                                                MHD_OPTION_HTTPS_PRIORITIES, "NORMAL",
                                                MHD_OPTION_END );
      }
      else {
         http->http_daemon = MHD_start_daemon(  http->server_type | MHD_USE_SSL, portnum, NULL, NULL, &md_HTTP_connection_handler, http,
                                                MHD_OPTION_HTTPS_MEM_KEY, server_key, 
                                                MHD_OPTION_HTTPS_MEM_CERT, server_cert, 
                                                MHD_OPTION_THREAD_POOL_SIZE, conf->num_http_threads,
                                                MHD_OPTION_NOTIFY_COMPLETED, md_HTTP_cleanup, http,
                                                MHD_OPTION_HTTPS_PRIORITIES, "NORMAL",
                                                MHD_OPTION_END );
      }
   
      if( http->http_daemon )
         dbprintf( "Started HTTP server with SSL enabled (cert = %s, pkey = %s) on port %d\n", conf->server_cert, conf->server_key, portnum);
   }
   else {
      // SSL disabled
      if( http->server_type == MHD_USE_THREAD_PER_CONNECTION ) {
         http->http_daemon = MHD_start_daemon(  http->server_type, portnum, NULL, NULL, &md_HTTP_connection_handler, http,
                                                MHD_OPTION_NOTIFY_COMPLETED, md_HTTP_cleanup, http,
                                                MHD_OPTION_END );
      }
      else {
         http->http_daemon = MHD_start_daemon(  http->server_type, portnum, NULL, NULL, &md_HTTP_connection_handler, http,
                                                MHD_OPTION_THREAD_POOL_SIZE, conf->num_http_threads,
                                                MHD_OPTION_NOTIFY_COMPLETED, md_HTTP_cleanup, http,
                                                MHD_OPTION_END );
      }
      
      if( http->http_daemon )
         dbprintf( "Started HTTP server on port %d\n", portnum);
   }
   
   if( http->http_daemon == NULL ) {
      pthread_rwlock_destroy( &http->lock );
      return -1;
   }
   
   return 0;
}

// stop the HTTP thread
int md_stop_HTTP( struct md_HTTP* http ) {
   MHD_stop_daemon( http->http_daemon );
   http->http_daemon = NULL;

   if( http->server_cert ) {
      free( http->server_cert );
      http->server_cert = NULL;
   }

   if( http->server_pkey ) {
      free( http->server_pkey );
      http->server_pkey = NULL;
   }
   
   return 0;
}

// free the HTTP server
int md_free_HTTP( struct md_HTTP* http ) {
   if( http->users ) {
      for( int i = 0; http->users[i] != NULL; i++ ) {
         md_free_user_entry( http->users[i] );
      }
      free( http->users );
      http->users = NULL;
   }
   pthread_rwlock_destroy( &http->lock );
   return 0;
}

int md_http_rlock( struct md_HTTP* http ) {
   return pthread_rwlock_rdlock( &http->lock );
}

int md_http_wlock( struct md_HTTP* http ) {
   return pthread_rwlock_wrlock( &http->lock );
}

int md_http_unlock( struct md_HTTP* http ) {
   return pthread_rwlock_unlock( &http->lock );
}

// parse a url_path--extract the file path, file version, block, block version, and/or manifest timestamp
// valid requests on file /foo/bar
//    /SYNDICATE-DATA/foo/bar.123/manifest.12345.67890   --> (/foo/bar, 123, -1, -1, 12345.67890, false)
//    /SYNDICATE-DATA/foo/bar/manifest.12345.67890       --> (/foo/bar, -1, -1, -1, 12345.67890, false)
//    /SYNDICATE-DATA/foo/bar.123/45.678                 --> (/foo/bar, 123, 45, 678, (-1,-1), false)
//    /SYNDICATE-DATA/foo/bar/45.678                     --> (/foo/bar, -1, 45, 678, (-1,-1), false)
//    /SYNDICATE-DATA/foo/bar.123/45                     --> (/foo/bar, 123, 45, -1, (-1,-1), false)
//    /SYNDICATE-DATA/foo/bar/45                         --> (/foo/bar, -1, 45, -1, (-1,-1), false)
//    /SYNDICATE-STAGING/foo/bar.123/45.678              --> (/foo/bar, 123, 45, 678, (-1,-1), true)
int md_HTTP_parse_url_path( char* _url_path, char** file_path, int64_t* file_version, uint64_t* block_id, int64_t* block_version, struct timespec* manifest_timestamp, bool* staging ) {
   char* url_path = strdup( _url_path );
   *staging = false;

   char* leaf = strrchr( url_path, '/' );
   if( leaf != NULL ) {
      if( *(leaf + 1) == '\0' ) {
         // request for a directory
         *file_path = NULL;
         *file_version = -1;
         *block_id = INVALID_BLOCK_ID;
         *block_version = -1;
         manifest_timestamp->tv_sec = -1;
         manifest_timestamp->tv_nsec = -1;
         free( url_path );
         return -EISDIR;
      }

      bool has_block = true;
      bool has_file_version = true;
      bool is_manifest = false;

      // is the leaf a manifest?
      if( strncmp( leaf + 1, "manifest", strlen("manifest") ) == 0 ) {

         // attempt to get the timestamp
         char* timestamp_begin = leaf + 1 + strlen("manifest");
         if( *timestamp_begin == '.' ) {
            timestamp_begin++;

            char* nsec_begin = NULL;
            long mts_sec = strtol( timestamp_begin, &nsec_begin, 10 );
            if( *nsec_begin == '.' && nsec_begin != timestamp_begin ) {
               // got the seconds; trying for nanoseconds
               nsec_begin++;
               char* tmp = NULL;
               long mts_nsec = strtol( nsec_begin, &tmp, 10 );

               if( *tmp == '\0' ) {
                  // this is a manifest!
                  is_manifest = true;
                  manifest_timestamp->tv_sec = mts_sec;
                  manifest_timestamp->tv_nsec = mts_nsec;

                  *leaf = 0;
               }
            }
         }
      }

      // is the leaf a block (or versioned block)?
      if( !is_manifest ) {
         char* version_ptr = strrchr( leaf, '.' );
         if( version_ptr != NULL ) {
            char* tmp1 = NULL, * tmp2 = NULL;

            // attempt to read the block ID and block version
            *block_id = (uint64_t)strtoll( leaf + 1, &tmp1, 10 );
            *block_version = (int64_t)strtoll( version_ptr + 1, &tmp2, 10 );

            if( tmp1 != version_ptr || *tmp2 != '\0' ) {
               // not [0-9]+.[0-9]+
               *block_id = INVALID_BLOCK_ID;
               *block_version = -1;
               has_block = false;
            }
         }
         else {
            // no version given
            *block_version = -1;

            // is this a block nevertheless?
            char* tmp1 = NULL;
            *block_id = (uint64_t)strtoll( leaf + 1, &tmp1, 10 );

            if( *tmp1 != '\0' ) {
               // not [0-9]+
               *block_id = INVALID_BLOCK_ID;
               has_block = false;
            }
         }
      }

      // if there was a block, then chop off the block text
      if( has_block )
         *leaf = 0;

      // is there a the file version?
      leaf = strrchr( url_path, '/' );
      if( leaf == NULL ) {
         // not possible
         *file_path = NULL;
         *file_version = -1;
         *block_id = INVALID_BLOCK_ID;
         *block_version = -1;
         manifest_timestamp->tv_sec = -1;
         manifest_timestamp->tv_nsec = -1;
         free( url_path );
         return -EINVAL;
      }
      char* version_ptr = strrchr( leaf, '.' );
      if( version_ptr != NULL ) {
         // a version was given
         char* tmp = NULL;
         *file_version = (int64_t)strtoll( version_ptr + 1, &tmp, 10 );
         if( *tmp != '\0' ) {
            // nope
            *file_version = -1;
            has_file_version = false;
         }
      }
      else {
         has_file_version = false;
      }

      // cut off the version
      if( has_file_version )
         *version_ptr = 0;

      if( strlen(url_path) > strlen(SYNDICATE_DATA_PREFIX) && strncmp( url_path, SYNDICATE_DATA_PREFIX, strlen(SYNDICATE_DATA_PREFIX) ) == 0 ) {
         // data URL
         char* tmp = strdup( url_path + strlen(SYNDICATE_DATA_PREFIX) - 1 );
         free( url_path );
         url_path = tmp;
      }
      else if( strlen(url_path) > strlen(SYNDICATE_STAGING_PREFIX) && strncmp( url_path, SYNDICATE_STAGING_PREFIX, strlen(SYNDICATE_STAGING_PREFIX) ) == 0 ) {
         // staging URL
         char* tmp = strdup( url_path + strlen(SYNDICATE_STAGING_PREFIX) - 1 );
         free( url_path );
         url_path = tmp;
         *staging = true;
      }
      
      else {
         // malformed
         *file_path = NULL;
         *file_version = -1;
         *block_id = INVALID_BLOCK_ID;
         *block_version = -1;
         manifest_timestamp->tv_sec = -1;
         manifest_timestamp->tv_nsec = -1;
         free( url_path );
         return -EINVAL;
      }
      *file_path = url_path;
      return 0;
   }
   else {
      *file_path = NULL;
      *file_version = -1;
      *block_id = INVALID_BLOCK_ID;
      *block_version = -1;
      free( url_path );
      return -EINVAL;
   }
}


struct md_user_entry** md_parse_secrets_file( char const* path ) {
   FILE* passwd_file = fopen( path, "r" );
   if( passwd_file == NULL ) {
      errorf( "Could not read password file %s, errno = %d\n", path, -errno );
      return NULL;
   }

   struct md_user_entry** ret = md_parse_secrets_file2( passwd_file, path );

   fclose( passwd_file );
   return ret;
}

// parse a secrets file
struct md_user_entry** md_parse_secrets_file2( FILE* passwd_file, char const* path ) {
   
   // format: UID:username:password_hash:public_url\n
   char* buf = NULL;
   size_t len = 0;
   vector<struct md_user_entry*> user_ents;
   int line_num = 0;
   
   while( true ) {
      ssize_t sz = getline( &buf, &len, passwd_file );
      line_num++;
      
      if( sz < 0 ) {
         // out of lines
         free( buf );
         break;
      }
      if( buf == NULL ) {
         // error
         errorf( "Error reading password file %s\n", path );
         return NULL;
      }
      if( strlen(buf) == 0 ) {
         // out of lines
         free( buf );
         break;
      }
      // parse the line
      char* save_buf = NULL;
      char* uid_str = strtok_r( buf, ":", &save_buf );
      char* username_str = strtok_r( NULL, ":", &save_buf );
      char* password_hash = strtok_r( NULL, "\n", &save_buf );
      
      // sanity check
      if( uid_str == NULL || username_str == NULL || password_hash == NULL ) {
         errorf( "Could not read password file %s: invalid line %d\n", path, line_num );
         return NULL;
      }
      
      // sanity check on UID
      char* next = NULL;
      uid_t uid = strtol( uid_str, &next, 10 );
      if( next == uid_str ) {
         errorf( "Could not read password file %s: invalid UID '%s' at line %d\n", path, uid_str, line_num );
         return NULL;
      }
      
      // save the data
      struct md_user_entry* uent = (struct md_user_entry*)calloc( sizeof(struct md_user_entry), 1 );
      uent->uid = uid;
      uent->username = strdup( username_str );
      uent->password_hash = strdup( password_hash );
      
      dbprintf( "%d %s %s\n", uid, username_str, password_hash);

      user_ents.push_back( uent );
   }
   
   // convert vector to NULL-terminated list
   struct md_user_entry** ret = (struct md_user_entry**)calloc( sizeof(struct md_user_entry*) * (user_ents.size() + 1), 1 );
   for( unsigned int i = 0; i < user_ents.size(); i++ ) {
      ret[i] = user_ents.at(i);
   }
   
   return ret;
}

// duplicate a user entry
struct md_user_entry* md_user_entry_dup( struct md_user_entry* uent ) {
   struct md_user_entry* ret = (struct md_user_entry*)calloc( sizeof(struct md_user_entry), 1 );
   
   if( uent->username )
      ret->username = strdup( uent->username );
   if( uent->password_hash )
      ret->password_hash = strdup( uent->password_hash );
   
   ret->uid = uent->uid;
   
   return ret;
}


// free the memory of a user entry
void md_free_user_entry( struct md_user_entry* uent ) {
   if( uent->username ) {
      free( uent->username );
      uent->username = NULL;
   }
   if( uent->password_hash ) {
      free( uent->password_hash );
      uent->password_hash = NULL;
   }
}


// flatten a response buffer into a byte string
char* response_buffer_to_string( response_buffer_t* rb ) {
   size_t total_len = 0;
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      total_len += (*rb)[i].second;
   }

   char* msg_buf = CALLOC_LIST( char, total_len );
   size_t offset = 0;
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      memcpy( msg_buf + offset, (*rb)[i].first, (*rb)[i].second );
      offset += (*rb)[i].second;
   }

   return msg_buf;
}


// free a response buffer
void response_buffer_free( response_buffer_t* rb ) {
   if( rb == NULL )
      return;
      
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      if( rb->at(i).first != NULL ) {
         free( rb->at(i).first );
         rb->at(i).first = NULL;
      }
      rb->at(i).second = 0;
   }
   rb->clear();
}

// size of a response buffer
size_t response_buffer_size( response_buffer_t* rb ) {
   size_t ret = 0;
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      ret += rb->at(i).second;
   }
   return ret;
}

// initialize Syndicate
int md_init( int gateway_type,
             char const* config_file,
             struct md_syndicate_conf* conf,
             struct ms_client* client,
             struct md_user_entry*** users,
             int portnum,
             char const* ms_url,
             char const* volume_name,
             char const* volume_secret,
             char const* md_username,
             char const* md_password ) {

   util_init();
   md_default_conf( conf );
   
   // read the config file
   int rc = md_read_conf( config_file, conf );
   if( rc != 0 ) {
      dbprintf("ERR: failed to read %s, rc = %d\n", config_file, rc );
      if( !(rc == -ENOENT || rc == -EACCES || rc == -EPERM) ) {
         // not just a simple "not found" or "permission denied"
         return rc;
      }  
   }
   
   // set up libsyndicate runtime information
   md_runtime_init( gateway_type, conf, NULL );

   // merge command-line options with the config....
   if( ms_url ) {
      if( conf->metadata_url )
         free( conf->metadata_url );

      conf->metadata_url = strdup( ms_url );
   }
   if( md_username ) {
      if( conf->metadata_username )
         free( conf->metadata_username );

      conf->metadata_username = strdup( md_username );
   }
   if( md_password ) {
      if( conf->metadata_password )
         free( conf->metadata_password );

      conf->metadata_password = strdup( md_password );
   }
   if( volume_name ) {
      if( conf->volume_name )
         free( conf->volume_name );

      conf->volume_name = strdup( volume_name );
   }
   if( portnum > 0 ) {
      conf->portnum = portnum;
   }

   if( conf->portnum <= 0 ) {
      errorf("invalid port number: conf's is %d, caller's is %d\n", conf->portnum, portnum);
      return -EINVAL;
   }

   // validate the config
   rc = md_check_conf( gateway_type, conf );
   if( rc != 0 ) {
      errorf("ERR: md_check_conf rc = %d\n", rc );
      return rc;
   }

   // connect to the MS
   rc = ms_client_init( client, conf, conf->volume_name, conf->metadata_username, conf->metadata_password );
   if( rc != 0 ) {
      errorf("ms_client_init rc = %d\n", rc );
      return rc;
   }

   // get the volume metadata
   rc = ms_client_get_volume_metadata( client, conf->volume_name, volume_secret, &conf->volume_version, &conf->owner, &conf->volume_owner, &conf->volume, &conf->replica_urls, &conf->blocking_factor, users );
   if( rc != 0 ) {
      errorf("ms_client_get_volume_metadata rc = %d\n", rc );
      return rc;
   }

   return 0;
}

// default configuration
int md_default_conf( struct md_syndicate_conf* conf ) {
   
   conf->default_read_freshness = 5000;
   conf->default_write_freshness = 5000;
   conf->logfile_path = NULL;       // filled by md_runtime_init
   conf->cdn_prefix = NULL;
   conf->proxy_url = NULL;
   conf->gather_stats = false;
   conf->use_checksums = false;
   conf->content_url = NULL;        // filled by md_runtime_init
   conf->data_root = NULL;          // filled by md_runtime_init
   conf->staging_root = NULL;       // filled by md_runtime_init
   conf->num_replica_threads = 1;
   conf->replica_logfile = NULL;    // filled by md_runtime_init
   conf->httpd_portnum = 44444;

   conf->verify_peer = true;
   conf->num_http_threads = 1;
   conf->http_authentication_mode = HTTP_AUTHENTICATE_READWRITE;
   conf->md_pidfile_path = NULL;
   conf->gateway_metadata_root = NULL;
   conf->replica_overwrite = false;
   conf->server_key = NULL;
   conf->server_cert = NULL;

   conf->debug_read = false;
   conf->debug_lock = false;

   conf->volume_name = NULL;
   conf->metadata_connect_timeout = 60;
   conf->portnum = 32780;
   conf->transfer_timeout = 60;

   conf->metadata_url = NULL;
   conf->metadata_username = NULL;
   conf->metadata_password = NULL;
   conf->replica_urls = NULL;
   conf->blocking_factor = 0;
   conf->owner = getuid();
   conf->usermask = 0377;
   conf->volume = 0;
   conf->volume_owner = 0;
   conf->mountpoint = NULL;      // filled by md_runtime_init
   conf->hostname = NULL;        // filled by md_runtime_init
   conf->volume_version = 0;

   return 0;
}


// check a configuration structure to see that it has everything we need.
// print warnings too
int md_check_conf( int gateway_type, struct md_syndicate_conf* conf ) {
   char const* warn_fmt = "WARN: missing configuration parameter: %s\n";
   char const* err_fmt = "ERR: missing configuration parameter: %s\n";

   // universal configuration warnings and errors
   int rc = 0;
   if( conf->logfile_path == NULL ) {
      rc = -EINVAL;
      fprintf(stderr, err_fmt, LOGFILE_PATH_KEY );
   }
   if( conf->cdn_prefix == NULL ) {
      fprintf(stderr, warn_fmt, CDN_PREFIX_KEY );
   }
   if( conf->proxy_url == NULL ) {
      fprintf(stderr, warn_fmt, PROXY_URL_KEY );
   }
   if( conf->content_url == NULL ) {
      fprintf(stderr, err_fmt, CONTENT_URL_KEY );
   }
   if( conf->content_url == NULL ) {
      rc = -EINVAL;
      fprintf(stderr, err_fmt, CONTENT_URL_KEY);
   }
   if( conf->metadata_url == NULL ) {
      rc = -EINVAL;
      fprintf(stderr, err_fmt, METADATA_URL_KEY );
   }
   if( conf->metadata_username == NULL ) {
      rc = -EINVAL;
      fprintf(stderr, err_fmt, METADATA_USERNAME_KEY );
   }
   if( conf->metadata_password == NULL ) {
      rc = -EINVAL;
      fprintf(stderr, err_fmt, METADATA_PASSWORD_KEY );
   }
   if( conf->volume_name == NULL ) {
      rc = -EINVAL;
      fprintf(stderr, err_fmt, VOLUME_NAME_KEY );
   }
   
   if( gateway_type == SYNDICATE_UG ) {
      // UG-specific warnings and errors
      if( conf->data_root == NULL ) {
         rc = -EINVAL;
         fprintf(stderr, err_fmt, DATA_ROOT_KEY );
      }
      if( conf->staging_root == NULL ) {
         rc = -EINVAL;
         fprintf(stderr, err_fmt, STAGING_ROOT_KEY );
      }
      if( conf->replica_urls == NULL || conf->replica_urls[0] == NULL ) {
         fprintf(stderr, "%s", "WARN: no RG URLs given\n");
      }
   }

   else {
      // RG/AG-specific warnings and errors
      if( conf->gateway_metadata_root == NULL ) {
         rc = -EINVAL;
         fprintf(stderr, err_fmt, GATEWAY_METADATA_KEY );
      }
      if( conf->server_key == NULL ) {
         fprintf(stderr, warn_fmt, SSL_PKEY_KEY );
      }
      if( conf->server_cert == NULL ) {
         fprintf(stderr, warn_fmt, SSL_CERT_KEY );
      }
   }

   return rc;
}