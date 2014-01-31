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

#include "opts.h"

// fill opts with defaults
int syndicate_default_opts( struct syndicate_opts* opts ) {
   memset( opts, 0, sizeof(struct syndicate_opts) );
   
   opts->config_file = (char*)CLIENT_DEFAULT_CONFIG;
   opts->flush_replicas = true;
   
   return 0;
}

// parse opts from argv.
// optionally supply the optind after parsing (if it's not NULL)
// return 0 on success
// return -1 on failure
int syndicate_parse_opts( struct syndicate_opts* opts, int argc, char** argv, int* out_optind, char const* special_opts, int (*special_opt_handler)(int, char*) ) {
   
   static struct option syndicate_options[] = {
      {"config-file",     required_argument,   0, 'c'},
      {"cdn-prefix",      required_argument,   0, 'x'},
      {"http-proxy",      required_argument,   0, 'X'},
      {"volume-name",     required_argument,   0, 'v'},
      {"username",        required_argument,   0, 'u'},
      {"password",        required_argument,   0, 'p'},
      {"gateway",         required_argument,   0, 'g'},
      {"MS",              required_argument,   0, 'm'},
      {"volume-pubkey",   required_argument,   0, 'V'},
      {"gateway-pkey",    required_argument,   0, 'G'},
      {"tls-pkey",        required_argument,   0, 'S'},
      {"tls-cert",        required_argument,   0, 'C'},
      {"no-flush-replicas", no_argument,       0, 'F'},
      {"storage-root",    required_argument,   0, 'r'},
      {0, 0, 0, 0}
   };

   int rc = 0;
   int opt_index = 0;
   int c = 0;
   
   char const* default_optstr = "c:v:u:p:P:m:Fg:V:G:S:C:x:X:";
   
   char* optstr = NULL;
   if( special_opts != NULL ) {
      optstr = CALLOC_LIST( char, strlen(default_optstr) + strlen(special_opts) + 1 );
      sprintf( optstr, "%s%s", default_optstr, special_opts );
   }
   else {
      optstr = (char*)default_optstr;
   }
   
   while(rc == 0 && (c = getopt_long(argc, argv, optstr, syndicate_options, &opt_index)) != -1) {
      switch( c ) {
         case 'v': {
            opts->volume_name = optarg;
            break;
         }
         case 'c': {
            opts->config_file = optarg;
            break;
         }
         case 'u': {
            opts->username = optarg;
            break;
         }
         case 'p': {
            opts->password = optarg;
            break;
         }
         case 'm': {
            opts->ms_url = optarg;
            break;
         }
         case 'g': {
            opts->gateway_name = optarg;
            break;
         }
         case 'F': {
            // don't flush replicas
            opts->flush_replicas = false;
            break;
         }
         case 'V': {
            opts->volume_pubkey_path = optarg;
            break;
         }
         case 'G': {
            opts->gateway_pkey_path = optarg;
            break;
         }
         case 'S': {
            opts->tls_pkey_path = optarg;
            break;
         }
         case 'C': {
            opts->tls_cert_path = optarg;
            break;
         }
         case 'x': {
            opts->CDN_prefix = optarg;
            break;
         }
         case 'X': {
            opts->proxy_url = optarg;
            break;
         }
         case 'r': {
            opts->storage_root = optarg;
            break;
         }
         default: {
            rc = -1;
            if( special_opt_handler ) {
               rc = special_opt_handler( c, optarg );
            }
            if( rc == -1 ) {
               fprintf(stderr, "Unrecognized option -%c", c );
               rc = -1;
            }
            break;
         }
      }
   }

   if( optstr != default_optstr ) {
      free( optstr );
   }
   
   if( out_optind != NULL ) {
      *out_optind = optind;
   }
   return rc;
}


// print usage and exit
void syndicate_common_usage( char* progname ) {
   fprintf(stderr, "\
Usage of %s\n\
Common Syndicate command-line options\n\
Required arguments:\n\
   -m, --MS MS_URL\n\
            URL to your Metadata Service\n\
   -u, --username USERNAME\n\
            Syndicate account username\n\
   -p, --password PASSWORD\n\
            Syndicate account password\n\
   -v, --volume VOLUME_NAME\n\
            Name of the Volume you are going to access\n\
   -g, --gateway GATEWAY_NAME\n\
            Name of this gateway\n\
   -G, --gateway-pkey GATEWAY_PRIVATE_KEY_PATH\n\
            Path to this gateway's private key\n\
\n\
Optional arguments:\n\
   -V, --volume-pubkey VOLUME_PUBLIC_KEY_PATH\n\
            Path to the Volume's metadata public key\n\
   -S, --tls-pkey TLS_PRIVATE_KEY_PATH\n\
            Path to this gateway's TLS private key\n\
   -C, --tls-cert TLS_CERTIFICATE_PATH\n\
            Path to this gateway's TLS certificate\n\
   -x, --cdn-prefix CDN_PREFIX\n\
            CDN prefix to use (e.g. http://vcoblitz.vicci.org:8008)\n\
   -X, --http-proxy HTTP_PROXY\n\
            HTTP proxy URL to use\n\
   -F, --no-flush-replicas\n\
            If given, flush all ongoing replicas before exiting\n\
   -r, --storage-root STORAGE_ROOT\n\
            Cache local state at a particular location\n\
\n", progname );
}
