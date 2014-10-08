/*
   Copyright 2014 The Trustees of Princeton University

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

#include "libsyndicate/ms/url.h"

// make a URL to a given MS request path
char* ms_client_url( char const* ms_url, uint64_t volume_id, char const* metadata_path ) {
   char volume_id_str[50];
   sprintf(volume_id_str, "%" PRIu64, volume_id);

   char* volume_md_path = md_fullpath( metadata_path, volume_id_str, NULL );

   char* url = md_fullpath( ms_url, volume_md_path, NULL );

   free( volume_md_path );

   return url;
}

// POST url for a file
char* ms_client_file_url( char const* ms_url, uint64_t volume_id ) {
   
   char volume_id_str[50];
   sprintf( volume_id_str, "%" PRIu64, volume_id );

   
   char* volume_file_path = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/FILE/") + 1 + strlen(volume_id_str) + 1 );

   sprintf( volume_file_path, "%s/FILE/%s", ms_url, volume_id_str);
   
   return volume_file_path;
}

// GET url for a file
char* ms_client_file_read_url( char const* ms_url, uint64_t volume_id, uint64_t file_id, int64_t version, int64_t write_nonce, int page_id ) {

   char volume_id_str[50];
   sprintf( volume_id_str, "%" PRIu64, volume_id );

   char file_id_str[50];
   sprintf( file_id_str, "%" PRIX64, file_id );

   char version_str[50];
   sprintf( version_str, "%" PRId64, version );

   char write_nonce_str[60];
   sprintf( write_nonce_str, "%" PRId64, write_nonce );

   size_t page_id_len = 0;
   
   if( page_id >= 0 ) {
      page_id_len = 32;
   }
   
   char* volume_file_path = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/FILE/RESOLVE/") + 1 + strlen(volume_id_str) + 1 + strlen(file_id_str) + 1 +
                                               strlen(version_str) + 1 + strlen(write_nonce_str) + 1 + page_id_len + 1 );

   sprintf( volume_file_path, "%s/FILE/RESOLVE/%s/%s/%s/%s", ms_url, volume_id_str, file_id_str, version_str, write_nonce_str );
   
   if( page_id_len > 0 ) {
      
      char page_id_buf[50];
      sprintf( page_id_buf, "?page_id=%d", page_id );
      
      strcat( volume_file_path, page_id_buf );
   }
   
   return volume_file_path;
}

// GETXATTR url 
char* ms_client_getxattr_url( char const* ms_url, uint64_t volume_id, uint64_t file_id, char const* xattr_name ) {
   
   char volume_id_str[50];
   sprintf( volume_id_str, "%" PRIu64, volume_id );

   char file_id_str[50];
   sprintf( file_id_str, "%" PRIX64, file_id );

   char* getxattr_path = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/FILE/GETXATTR/") + 1 + strlen(volume_id_str) + 1 + strlen(file_id_str) + 1 + strlen(xattr_name) + 1 );
   
   sprintf( getxattr_path, "%s/FILE/GETXATTR/%s/%s/%s", ms_url, volume_id_str, file_id_str, xattr_name );
   
   return getxattr_path;
}

// LISTXATTR url 
char* ms_client_listxattr_url( char const* ms_url, uint64_t volume_id, uint64_t file_id ) {
   
   char volume_id_str[50];
   sprintf( volume_id_str, "%" PRIu64, volume_id );

   char file_id_str[50];
   sprintf( file_id_str, "%" PRIX64, file_id );

   char* listxattr_path = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/FILE/LISTXATTR/") + 1 + strlen(volume_id_str) + 1 + strlen(file_id_str) + 1 );
   
   sprintf( listxattr_path, "%s/FILE/LISTXATTR/%s/%s", ms_url, volume_id_str, file_id_str );
   
   return listxattr_path;
}

// URL to read a file's vacuum log
char* ms_client_vacuum_url( char const* ms_url, uint64_t volume_id, uint64_t file_id ) {
   
   char volume_id_str[50];
   sprintf( volume_id_str, "%" PRIu64, volume_id );

   char file_id_str[50];
   sprintf( file_id_str, "%" PRIX64, file_id );

   char* vacuum_path = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/FILE/VACUUM/") + 1 + strlen(volume_id_str) + 1 + strlen(file_id_str) + 1 );
   
   sprintf( vacuum_path, "%s/FILE/VACUUM/%s/%s", ms_url, volume_id_str, file_id_str );
   
   return vacuum_path;
}
   

// URL to a Volume, by ID
char* ms_client_volume_url( char const* ms_url, uint64_t volume_id ) {
   char buf[50];
   sprintf(buf, "%" PRIu64, volume_id );

   char* volume_md_path = md_fullpath( "/VOLUME/", buf, NULL );

   char* url = md_fullpath( ms_url, volume_md_path, NULL );

   free( volume_md_path );

   return url;
}

// URL to a Volume, by name
char* ms_client_volume_url_by_name( char const* ms_url, char const* name ) {
   char* volume_md_path = md_fullpath( "/VOLUME/", name, NULL );

   char* url = md_fullpath( ms_url, volume_md_path, NULL );
   
   free( volume_md_path );

   return url;
}

// URL to register with the MS, using a gateway keypair
char* ms_client_public_key_register_url( char const* ms_url ) {
   char* url = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/REGISTER/") + 1 );
   
   sprintf( url, "%s/REGISTER", ms_url );
   
   return url;
}
   
// URL to register with the MS, using an OpenID username/password
char* ms_client_openid_register_url( char const* ms_url, int gateway_type, char const* gateway_name, char const* username ) {
   // build the /REGISTER/ url

   char gateway_type_str[10];
   ms_client_gateway_type_str( gateway_type, gateway_type_str );

   char* url = CALLOC_LIST( char, strlen(ms_url) + 1 +
                                  strlen("/REGISTER/") + 1 +
                                  strlen(gateway_name) + 1 +
                                  strlen(username) + 1 +
                                  strlen(gateway_type_str) + 1 +
                                  strlen("/begin") + 1);

   sprintf(url, "%s/REGISTER/%s/%s/%s/begin", ms_url, gateway_type_str, gateway_name, username );

   return url;
}

// URL to perform an RPC with the MS, using OpenID to authenticate
char* ms_client_openid_rpc_url( char const* ms_url ) {
   // build the /API/ url for OpenID
   
   char* url = CALLOC_LIST( char, strlen(ms_url) + 1 +
                                  strlen("/API/begin") + 1);

   sprintf(url, "%s/API/begin", ms_url );

   return url;
}

// URL to fetch the MS's public key
char* ms_client_syndicate_pubkey_url( char const* ms_url ) {
   // build the /PUBKEY url 
   
   char* url = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/PUBKEY") + 1 );
   
   sprintf(url, "%s/PUBKEY", ms_url );
   
   return url;
}

// URL to a certificate manifest 
char* ms_client_cert_manifest_url( char const* ms_url, uint64_t volume_id, uint64_t volume_cert_version ) {
   
   char* url = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/CERT/") + 1 + 21 + 1 + strlen("manifest.") + 21 + 1 );
   sprintf(url, "%s/CERT/%" PRIu64 "/manifest.%" PRIu64, ms_url, volume_id, volume_cert_version );
   
   return url;
}


// get a certificate URL
char* ms_client_cert_url( char const* ms_url, uint64_t volume_id, uint64_t volume_cert_version, int gateway_type, uint64_t gateway_id, uint64_t gateway_cert_version ) {
   char type_str[5];
   ms_client_gateway_type_str( gateway_type, type_str );
   
   char* url = CALLOC_LIST( char, strlen(ms_url) + 1 + strlen("/CERT/") + 1 + 21 + 1 + 21 + 1 + strlen(type_str) + 1 + 21 + 1 + 21 + 1 );
   sprintf( url, "%s/CERT/%" PRIu64 "/%" PRIu64 "/%s/%" PRIu64 "/%" PRIu64, ms_url, volume_id, volume_cert_version, type_str, gateway_id, gateway_cert_version );
   
   return url;
}