/*
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
*/

#include <driver.h>

// server config 
//struct md_syndicate_conf CONF;
 
// set of files we're exposing
content_map DATA;

// set of files we map to SQL queries
query_map* FS2SQL = NULL;

// volumes set
volume_set* VOLUMES = NULL;

// Metadata service client of the AG
ms_client *mc = NULL;

// Location of local files we're exposing 
char *datapath = NULL;

// Length of datapath varaiable
size_t  datapath_len = 0;

// MapParser object 
MapParser *mp = NULL;

// ODBC DSN string
unsigned char* dsn_string = NULL;

// ReadWrite lock to protect DATA, FS2SQL etc.
pthread_rwlock_t driver_lock;

// Reversion daemon
ReversionDaemon* revd = NULL;

// true if init() is called
bool initialized = false;

// generate a manifest for an existing file, putting it into the gateway context
extern "C" int gateway_generate_manifest( struct gateway_context* replica_ctx, 
					    struct gateway_ctx* ctx, struct md_entry* ent ) {
    // No need of locking...
    errorf("%s", "INFO: gateway_generate_manifest\n"); 
    // populate a manifest
    Serialization::ManifestMsg* mmsg = new Serialization::ManifestMsg();
    mmsg->set_size( ent->size );
    mmsg->set_file_version( 1 );
    mmsg->set_mtime_sec( ent->mtime_sec );
    mmsg->set_mtime_nsec( 0 );
    mmsg->set_manifest_mtime_sec( ent->mtime_sec );
    mmsg->set_manifest_mtime_nsec( 0 );

    uint64_t num_blocks = ent->size / global_conf->blocking_factor;
    if( ent->size % global_conf->blocking_factor != 0 )
	num_blocks++;

    Serialization::BlockURLSetMsg *bbmsg = mmsg->add_block_url_set();
    bbmsg->set_start_id( 0 );
    bbmsg->set_end_id( num_blocks );
    stringstream strstrm;
    strstrm << ent->url << ent->path;
    bbmsg->set_file_url( strstrm.str() );

    for( uint64_t i = 0; i < num_blocks; i++ ) {
	bbmsg->add_block_versions( 0 );
    }

    // serialize
    string mmsg_str;
    bool src = mmsg->SerializeToString( &mmsg_str );
    if( !src ) {
	// failed
	errorf( "%s", "failed to serialize" );
	delete mmsg;
	return -EINVAL;
    }

    ctx->data_len = mmsg_str.size();
    ctx->data = CALLOC_LIST( char, mmsg_str.size() );
    replica_ctx->last_mod = ent->mtime_sec;
    memcpy( ctx->data, mmsg_str.data(), mmsg_str.size() );

    delete mmsg;

    return 0;
}


// read dataset or manifest 
extern "C" ssize_t get_dataset( struct gateway_context* dat, char* buf, size_t len, void* user_cls ) {
    DRIVER_RDONLY(&driver_lock);
    errorf("%s", "INFO: get_dataset\n"); 
    ssize_t ret = 0;
    ODBCHandler& odh = ODBCHandler::get_handle(dsn_string);
    struct gateway_ctx* ctx = (struct gateway_ctx*)user_cls;

    if( ctx->request_type == GATEWAY_REQUEST_TYPE_LOCAL_FILE ) {
	// read from database using ctx->sql_query...
	if (ctx->is_db_info && !ctx->complete) {
	    stringstream info;
	    string tbl_info = odh.get_tables();
	    string db_info = odh.get_db_info();
	    info<<"Database"<<endl<<"========"<<endl<<db_info<<endl;
	    info<<"Tables"<<endl<<"======"<<endl<<tbl_info<<endl;
	    string info_str = info.str();
	    const char* info_c_str = info_str.c_str();
	    size_t info_len = info.str().length();
	    memcpy(buf, info_c_str, info_len);
	    ret = info_len;
	    ctx->complete = true;
	}
	else if (!ctx->complete) {
	    if (ctx->data == NULL)
		odh.execute_query(ctx, ctx->mi, global_conf->blocking_factor);
	    if (ctx->data_len) {
		size_t rem_len = ctx->data_len - ctx->data_offset;
		size_t read_len = (rem_len > len)?len:rem_len;
		ctx->complete  = (rem_len > len)?false:true;
		memcpy(buf, ctx->data + ctx->data_offset, read_len);
		ret = read_len;
		ctx->data_offset += ret;
	    }
	    else {
		ret = 0;
		ctx->complete = true;
	    }
	}
	else if (ctx->complete) {
	    ret = 0;
	}
    }
    else if( ctx->request_type == GATEWAY_REQUEST_TYPE_MANIFEST ) {
	// read from RAM
	memcpy( buf, ctx->data + ctx->data_offset, MIN( (ssize_t)len, ctx->data_len - ctx->data_offset ) );
	ctx->data_offset += len;
	ret = (ssize_t)len;
    }
    else {
	// invalid structure
	ret = -EINVAL;
    }
    DRIVER_RETURN(ret, &driver_lock);
}


// get metadata for a dataset
extern "C" int metadata_dataset( struct gateway_context* dat, ms::ms_gateway_blockinfo* info, void* usercls ) {
    DRIVER_RDONLY(&driver_lock);
    errorf("%s", "INFO: metadata_dataset\n"); 
    char* file_path = NULL;
    int64_t file_version = 0;
    uint64_t block_id = 0;
    int64_t block_version = 0;
    struct timespec manifest_timestamp;
    manifest_timestamp.tv_sec = 0;
    manifest_timestamp.tv_nsec = 0;
    bool staging = false;

    int rc = md_HTTP_parse_url_path( (char*)dat->url_path, &file_path, &file_version, &block_id, &block_version, &manifest_timestamp, &staging );
    if( rc != 0 ) {
	errorf( "failed to parse '%s', rc = %d\n", dat->url_path, rc );
	free( file_path );
	DRIVER_RETURN(-EINVAL, &driver_lock);
    }

    content_map::iterator itr = DATA.find( string(file_path) );
    if( itr == DATA.end() ) {
	// not found in this volume
	DRIVER_RETURN(-ENOENT, &driver_lock);
    }

    struct gateway_ctx* ctx = (struct gateway_ctx*)usercls;
    struct md_entry* ent = itr->second;

    info->set_progress( ms::ms_gateway_blockinfo::COMMITTED );     // ignored, but needs to be filled in
    info->set_blocking_factor( global_conf->blocking_factor );

    info->set_file_version( file_version );
    info->set_block_id( ctx->block_id );
    info->set_block_version( block_version );
    info->set_fs_path( string(ctx->file_path) );
    info->set_file_mtime_sec( ent->mtime_sec );
    info->set_file_mtime_nsec( ent->mtime_nsec );
    info->set_write_time( ent->mtime_sec );

    DRIVER_RETURN(0, &driver_lock);
}


// interpret an inbound GET request
extern "C" void* connect_dataset( struct gateway_context* replica_ctx ) {
   DRIVER_RDONLY(&driver_lock);
   errorf("%s", "INFO: connect_dataset\n"); 
   char* file_path = NULL;
   int64_t file_version = 0;
   uint64_t block_id = 0;
   int64_t block_version = 0;
   struct timespec manifest_timestamp;
   manifest_timestamp.tv_sec = 0;
   manifest_timestamp.tv_nsec = 0;
   bool staging = false;

   int rc = md_HTTP_parse_url_path( (char*)replica_ctx->url_path, &file_path, &file_version, &block_id, &block_version, &manifest_timestamp, &staging );
   if( rc != 0 ) {
       errorf( "failed to parse '%s', rc = %d\n", replica_ctx->url_path, rc );
       free( file_path );
       DRIVER_RETURN(NULL, &driver_lock);
   }

   if( staging ) {
       errorf("invalid URL path %s\n", replica_ctx->url_path );
       free( file_path );
       DRIVER_RETURN(NULL, &driver_lock);
   }

   struct gateway_ctx* ctx = CALLOC_LIST( struct gateway_ctx, 1 );

   // is there metadata for this file?
   string fs_path( file_path );
   content_map::iterator itr = DATA.find( fs_path );
   if( itr == DATA.end() ) {
       // no entry; nothing to do
       free( file_path );
       DRIVER_RETURN(NULL, &driver_lock);
   }

   // default fo is_db_info is false
   ctx->is_db_info = false;
   // complete is initially false
   ctx->complete = false;
   // if this is a regarding "/.db_info"
   if (strstr(fs_path.c_str(), ".db_info"))
       ctx->is_db_info = true;
   struct md_entry* ent = DATA[ file_path ];

   // is this a request for a manifest?
   if( manifest_timestamp.tv_sec > 0 ) {
       // request for a manifest
       int rc = gateway_generate_manifest( replica_ctx, ctx, ent );
       if( rc != 0 ) {
	   // failed
	   errorf( "gateway_generate_manifest rc = %d\n", rc );

	   // meaningful error code
	   if( rc == -ENOENT )
	       replica_ctx->err = -404;
	   else if( rc == -EACCES )
	       replica_ctx->err = -403;
	   else
	       replica_ctx->err = -500;

	   free( ctx );
	   free( file_path );
	   DRIVER_RETURN(NULL, &driver_lock);
       }

       ctx->request_type = GATEWAY_REQUEST_TYPE_MANIFEST;
       ctx->data_offset = 0;
       ctx->block_id = 0;
       //ctx->num_read = 0;
       replica_ctx->size = ctx->data_len;
   }
   else {
       struct map_info *mi = NULL;
       query_map::iterator itr = FS2SQL->find(string(file_path));
       if (itr != FS2SQL->end()) {
	   mi = itr->second;
	   ctx->sql_query_bounded = mi->query; 
	   ctx->sql_query_unbounded = mi->unbounded_query; 
	   if( !ctx->sql_query_bounded ) {
	       free( ctx );
	       free( file_path );
	       replica_ctx->http_status = 404;
	       DRIVER_RETURN(NULL, &driver_lock);
	   }
	   else {
	       // set up for reading
	       ctx->data_offset = 0;
	       //ctx->num_read = 0;
	       ctx->block_id = block_id;
	       ctx->data = NULL;
	       ctx->request_type = GATEWAY_REQUEST_TYPE_LOCAL_FILE;
	       ctx->mi = mi;
	       // Negative size switches libmicrohttpd to chunk transfer mode
	       replica_ctx->size = -1;
	       //TODO: Check the block status and set the http response appropriately

	       replica_ctx->http_status = 200;
	   }
       }
       else {
	   replica_ctx->http_status = 404;
       }
   }

   ctx->file_path = file_path;
   DRIVER_RETURN(ctx, &driver_lock);
}


// clean up a transfer 
extern "C" void cleanup_dataset( void* cls ) {   
    // Locking not required
    errorf("%s", "INFO: cleanup_dataset\n"); 
    struct gateway_ctx* ctx = (struct gateway_ctx*)cls;
    if (ctx) {
	if (ctx->data != NULL) {
	    free(ctx->data);
	}
	free (ctx);
    }
}

extern "C" int publish_dataset (struct gateway_context*, ms_client *client, 
	char* dataset ) {
    if (mc == NULL)
	mc = client;
    if (mp == NULL)
	mp = new MapParser(dataset);
    map<string, struct map_info*>::iterator iter;
    set<char*, path_comp> dir_hierachy;
    mp->parse();
    unsigned char* dsn = mp->get_dsn();
    init(dsn);
    //DRIVER_RDWR should strictly be called after init.
    DRIVER_RDWR(&driver_lock);
    set<string> *volset = mp->get_volume_set();
    map<string, struct map_info*> *fs_map = mp->get_map();
    if (FS2SQL == NULL) {
	FS2SQL = new map<string, struct map_info*>(*fs_map);
    }
    else {
	update_fs_map(fs_map, FS2SQL, driver_special_inval_handler);
	//Delete map only, objects in the map are kept intact
	delete fs_map;
    }
    if (VOLUMES == NULL){
	VOLUMES = new set<string>(*volset);
    }
    else {
	//Delete set only, objects in the set are kept intact
	update_volume_set(volset, VOLUMES, NULL);
	delete volset;
    }
    for (iter = FS2SQL->begin(); iter != FS2SQL->end(); iter++) {
	const char* full_path = iter->first.c_str();
	char* path = strrchr((char*)full_path, '/');
	size_t dir_path_len = path - full_path;
	char* dir_path = (char*)malloc(dir_path_len + 1);
	dir_path[dir_path_len] = 0;
	strncpy(dir_path, full_path, dir_path_len);
	dir_hierachy.insert(dir_path);
    }
    set<char*, path_comp>::iterator it;
    for( it = dir_hierachy.begin(); it != dir_hierachy.end(); it++ ) {
	struct map_info mi;
	publish (*it, MD_ENTRY_DIR, &mi);
    }

    for (iter = FS2SQL->begin(); iter != FS2SQL->end(); iter++) {
	publish (iter->first.c_str(), MD_ENTRY_FILE, iter->second);
	if (revd)
	    revd->add_map_info(iter->second);
    }
    ms_client_destroy(mc);
    DRIVER_RETURN(0, &driver_lock);
}

static int publish(const char *fpath, int type, struct map_info* mi)
{
    //Called by publish_dataset, therefore locking neede.
    int i = 0;
    struct md_entry* ment = NULL;
    size_t len = strlen(fpath);
    char *path = NULL;
    size_t content_url_len = 0;
    //size_t local_proto_len = strlen( SYNDICATEFS_AG_DB_PROTO ); 
    //size_t url_len = local_proto_len + len;
    if ( len < datapath_len ) { 
	return -EINVAL;
    }
    if ( len == datapath_len ) {
	return 0;
    }
    //Set volume path
    size_t path_len = ( len - datapath_len ) + 1; 
    path = (char*)malloc( path_len );
    memset( path, 0, path_len );
    strncpy( path, fpath + datapath_len, path_len );

    if ((ment = DATA[path]) == NULL) {
	ment = new struct md_entry;
	ment->version = 1;
	ment->path = path;
	DATA[ment->path] = ment;

	//Set primary replica 
	content_url_len = strlen(global_conf->content_url);
	ment->url = ( char* )malloc( content_url_len + 1);
	memset( ment->url, 0, content_url_len + 1 );
	strncpy( ment->url, global_conf->content_url, content_url_len ); 

	ment->checksum = NULL;
	ment->local_path = NULL;
    }
    else { 
	free(path); 
    }

    //Set time from the real time clock
    struct timespec rtime; 
    if ((i = clock_gettime(CLOCK_REALTIME, &rtime)) < 0) {
	errorf("Error: clock_gettime: %i\n", i); 
	return -1;
    }

    ment->ctime_sec = rtime.tv_sec;
    ment->ctime_nsec = rtime.tv_nsec;
    ment->mtime_sec = rtime.tv_sec;
    ment->mtime_nsec = rtime.tv_nsec;
    ment->mode = mi->file_perm;
    ment->max_read_freshness = 1000;
    ment->max_write_freshness = 1;
    ment->volume = mc->conf->volume;
    ment->owner = mc->conf->volume_owner;
    switch (type) {
	case MD_ENTRY_DIR:
	    ment->size = 4096;
	    ment->type = MD_ENTRY_DIR;
	    ment->mode = DIR_PERMISSIONS_MASK;
	    ment->mode |= S_IFDIR;
	    if ( (i = ms_client_mkdir(mc, ment)) < 0 ) {
		cout<<"ms client mkdir "<<i<<endl;
	    }
	    break;
	case MD_ENTRY_FILE:
	    ment->size = -1;
	    ment->type = MD_ENTRY_FILE;
	    ment->mode &= FILE_PERMISSIONS_MASK;
	    ment->mode |= S_IFREG;
	    if ( (i = ms_client_create(mc, ment)) < 0 ) {
		cout<<"ms client create "<<i<<endl;
	    }
	    mi->mentry = ment;
	    mi->reversion_entry = reversion;
	    break;
	default:
	    break;
    }
    DATA[ment->path] = ment;
    //pfunc_exit_code = 0;
    return 0;  
}


void init(unsigned char* dsn) {
    if (!initialized)
	initialized = true;
    else
	return;
    if (dsn_string == NULL) {
	size_t dsn_len = strlen((const char*)dsn) + strlen((const char*)ODBC_DSN_PREFIX);
	dsn_string = (unsigned char*)malloc(dsn_len + 1);
	memset(dsn_string, 0, dsn_len + 1);
	memcpy(dsn_string, ODBC_DSN_PREFIX, strlen((const char*)ODBC_DSN_PREFIX));
	memcpy(dsn_string + strlen((const char*)ODBC_DSN_PREFIX), dsn, strlen((const char*)dsn));
    }
    if (revd == NULL) {
	revd = new ReversionDaemon();
	revd->run();
    }
    add_driver_event_handler(DRIVER_RECONF, reconf_handler, NULL);
    driver_event_start();
    if (pthread_rwlock_init(&driver_lock, NULL) < 0) {
	perror("pthread_rwlock_init");
	exit(-1);
    }
}

void reversion(void *cls) {
    struct md_entry *ment = (struct md_entry*)cls;
    if (ment == NULL)
	return;
    ment->version++;
    ms_client_update(mc, ment);
}


void* reconf_handler(void *cls) {
    cout<<"calling publish_dataset"<<endl;
    publish_dataset (NULL, NULL, NULL );
    return NULL;
}

void driver_special_inval_handler(string file_path) {
    struct md_entry *mde = DATA[file_path];
    if (mde != NULL) {
	cout<<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>... "<<mde->path<<endl;
	//Delete this file from all the volumes we are attached to.
	ms_client_delete(mc, mde);
	DATA.erase(file_path);
	if (mde->url)
	    free(mde->url);
	if (mde->path)
	    free(mde->path);
	free(mde);
    }
    //Remove from reversion daemon
    struct map_info* mi = (*FS2SQL)[file_path];
    if (mi != NULL) {
	revd->remove_map_info(mi);
    }
    //NOTE: Do not delete mi, it will be deleted by map-parser.c::
    //update_fs_map.
}

