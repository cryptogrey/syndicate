#!/usr/bin/env python

"""
   Copyright 2013 The Trustees of Princeton University
   All Rights Reserved
"""

import webapp2
import urlparse

import MS
from MS.methods.resolve import Resolve
import MS.methods.common

import protobufs.ms_pb2 as ms_pb2
import protobufs.serialization_pb2 as serialization_pb2

from storage import storage
import storage.storagetypes as storagetypes

import api
from entry import MSEntry
from volume import Volume
from gateway import *
from msconfig import *

import errno
import logging
import random
import os
import base64
import urllib
import time
import cgi
import datetime
import rpc.jsonrpc

from openid.gaeopenid import GAEOpenIDRequestHandler
from openid.consumer import consumer
import openid.oidutil

HTTP_MS_LASTMOD = "X-MS-LastMod"

def get_client_lastmod( headers ):
   lastmod = headers.get( HTTP_MS_LASTMOD )
   if lastmod == None:
      return None

   try:
      lastmod_f = float( lastmod )
      return lastmod_f
   except:
      return None

   

def read_gateway_basic_auth( headers ):
   basic_auth = headers.get("Authorization")
   if basic_auth == None:
      logging.info("no authorization header")
      return (None, None, None)

   # basic auth format:
   # ${gateway_type}_${gateway_id}:${password}
   # example:
   # UG_3:01234567890abcdef

   gateway_type, gateway_id, password = '', '', ''
   try:
      user_info = base64.decodestring( basic_auth[6:] )
      gateway, password = user_info.split(":")
      gateway_type, gateway_id = gateway.split("_")
      gateway_id = int(gateway_id)
   except:
      logging.info("incomprehensible Authorization header: '%s'" % basic_auth )
      return (None, None, None)

   return gateway_type, gateway_id, password



def response_load_gateway_by_type_and_id( gateway_type, gateway_id ):
   if gateway_id == None:
      # invalid header
      return (None, 403, None)

   gateway_read_start = storagetypes.get_time()
   gateway = None

   # get the gateway and validate its type
   if gateway_type == "UG":
      gateway = UserGateway.Read( gateway_id )
   elif gateway_type == "RG":
      gateway = ReplicaGateway.Read( gateway_id )
   elif gateway_type == "AG":
      gateway = AcquisitionGateway.Read( gateway_id )
   else:
      return (None, 401, None)

      
   gateway_read_time = storagetypes.get_time() - gateway_read_start
   return (gateway, 200, gateway_read_time)


def response_load_volume( volume_name_or_id ):

   volume_id = -1
   try:
      volume_id = int( volume_name_or_id )
   except:
      pass

   volume_read_start = storagetypes.get_time()

   volume = None
   if volume_id > 0:
      volume = storage.read_volume( volume_id )
   else:
      volume = storage.get_volume_by_name( volume_name_or_id )

   volume_read_time = storagetypes.get_time() - volume_read_start

   if volume == None:
      # no volume
      return (None, 404, None)

   if not volume.active:
      # inactive volume
      return (None, 503, None)

   return (volume, 200, volume_read_time)


def response_volume_error( request_handler, status ):

   request_handler.response.status = status
   request_handler.response.headers['Content-Type'] = "text/plain"
   
   if status == 404:
      # no volume
      request_handler.response.write("No such volume\n")

   elif status == 503:
      # inactive volume
      request_handler.response.write("Service Not Available\n")

   return
   

def response_server_error( request_handler, status, msg=None ):
   
   request_handler.response.status = status
   request_handler.response.headers['Content-Type'] = "text/plain"

   if status == 500:
      # server error
      if msg == None:
         msg = "Internal Server Error"
      request_handler.response.write( msg )

   return
   

def response_user_error( request_handler, status, message=None ):

   request_handler.response.status = status
   request_handler.response.headers['Content-Type'] = "text/plain"

   if status == 400:
      if message == None:
         messsage = "Invalid Request\n"
      request_handler.response.write(message)
      
   elif status == 404:
      if message == None:
         messsage = "No such gateway\n"
      request_handler.response.write(message)
      
   elif status == 401:
      if message == None:
         message = "Authentication required\n"
      request_handler.response.write(message)

   elif status == 403:
      if message == None:
         message = "Authorization Failed\n"
      request_handler.response.write(message)

   return


def response_load_gateway( request_handler ):
   # get the gateway's credentials
   gateway_type_str, g_id, password = read_gateway_basic_auth( request_handler.request.headers )

   if gateway_type_str == None or g_id == None or password == None:
      response_user_error( request_handler, 401 )
      return (None, None, None)

   # look up the requesting gateway
   gateway, status, gateway_read_time = response_load_gateway_by_type_and_id( gateway_type_str, g_id )

   if status != 200:
      response_user_error( request_handler, status )
      return (None, None, None)

   # make sure this gateway is legit, if needed
   valid_gateway = gateway.authenticate_session( password )

   if not valid_gateway:
      # invalid credentials
      logging.error("Invalid session credentials")
      response_user_error( request_handler, 403 )
      return (None, None, None)

   return (gateway, status, gateway_read_time)
   

def response_begin( request_handler, volume_name_or_id ):
   
   timing = {}
   
   timing['request_start'] = storagetypes.get_time()

   # get the Volume
   volume, status, volume_read_time = response_load_volume( volume_name_or_id )

   if status != 200:
      response_volume_error( request_handler, status )
      return (None, None, None)

   gateway_read_time = 0
   gateway = None
   
   if volume.need_gateway_auth():
      # authenticate the gateway
      gateway, status, gateway_read_time = response_load_gateway( request_handler )

      if status != 200:
         return (None, None, None)
      
   # make sure this gateway is allowed to access this Volume
   valid_gateway = volume.is_gateway_in_volume( gateway )
   if not valid_gateway:
      # gateway does not belong to this Volume
      logging.error("Not in this Volume")
      response_user_error( request_handler, 403 )
      return (None, None, None)

   # if we're still here, we're good to go

   timing['X-Volume-Time'] = str(volume_read_time)
   timing['X-Gateway-Time'] = str(gateway_read_time)
   
   return (gateway, volume, timing)


   
def response_end( request_handler, status, data, content_type=None, timing=None ):
   if content_type == None:
      content_type = "application/octet-stream"

   if timing != None:
      request_total = storagetypes.get_time() - timing['request_start']
      timing['X-Total-Time'] = str(request_total)
      
      del timing['request_start']
      
      for (time_header, time) in timing.items():
         request_handler.response.headers[time_header] = time

   request_handler.response.headers['Content-Type'] = content_type
   request_handler.response.status = status
   request_handler.response.write( data )
   return
   
   
   

class MSVolumeRequestHandler(webapp2.RequestHandler):
   """
   Volume metadata request handler.
   """

   def get( self, volume_id_str ):
      gateway, volume, timing = response_begin( self, volume_id_str )
      if volume == None:
         return

      if volume.need_gateway_auth() and gateway == None:
         response_user_error( self, 403 )
         return 
      
      # request for volume metadata
      volume_metadata = ms_pb2.ms_volume_metadata();
      volume.protobuf( volume_metadata )
      data = volume_metadata.SerializeToString()

      response_end( self, 200, data, "application/octet-stream", timing )
      return


class MSCertManifestRequestHandler( webapp2.RequestHandler ):
   """
   Certificate bundle manifest request handler
   """
   
   def get( self, volume_id_str, volume_cert_version_str ):
      volume_cert_version = 0
      try:
         volume_cert_version = int( volume_cert_version_str )
      except:
         response_end( self, 400, "Invalid Request", "text/plain" )
         return
      
      # get the Volume
      volume, status, _ = response_load_volume( volume_id_str )

      if status != 200:
         response_volume_error( self, status )
         return
      
      # check version
      if volume_cert_version != volume.cert_version:
         hdr = "%s/CERT/%s/manifest.%s" % (MS_URL, volume_id_str, volume.cert_version)
         self.response.headers['Location'] = hdr
         response_end( self, 302, "Location: %s" % hdr, "text/plain" )
         return

      # build the manifest
      manifest = serialization_pb2.ManifestMsg()
      
      volume.protobuf_gateway_cert_manifest( manifest, [UserGateway, ReplicaGateway, AcquisitionGateway] )
      
      data = manifest.SerializeToString()
      
      response_end( self, 200, data, "application/octet-stream" )
      return
      
      

class MSCertRequestHandler( webapp2.RequestHandler ):
   """
   Certificate bundle request handler.
   """
   
   def get( self, volume_id_str, volume_cert_version_str, gateway_type_str, gateway_id_str, gateway_cert_version_str ):
      gateway_id = 0
      volume_cert_version = 0
      gateway_cert_version = 0
      
      try:
         gateway_id = int( gateway_id_str )
         gateway_cert_version = int( gateway_cert_version_str )
         volume_cert_version = int( volume_cert_version_str )
      except:
         response_end( self, 400, "Invalid Request", "text/plain" )
         return
      
      
      # get the Volume
      volume, status, _ = response_load_volume( volume_id_str )

      if status != 200:
         response_volume_error( self, status )
         return

      # request the gateway
      gateway = None
      if gateway_type_str == "UG":
         gateway = storage.read_user_gateway( gateway_id )
      elif gateway_type_str == "RG":
         gateway = storage.read_replica_gateway( gateway_id )
      elif gateway_type_str == "AG":
         gateway = storage.read_acquisition_gateway( gateway_id )
      else:
         logging.error("Invalid gateway type '%s'" % gateway_type_str )
         response_user_error( self, 401 )
         return
         
      if gateway == None:
         logging.error("No such %s named %s" % (gateway_type_str, gateway_name))
         response_user_error( self, 404 )
         return
     
      # request only the right version
      if volume_cert_version != volume.cert_version or gateway_cert_version != gateway.cert_version:
         hdr = "%s/CERT/%s/%s/%s/%s/%s" % (MS_URL, volume_id_str, volume.cert_version, gateway_type_str, gateway_id_str, gateway.cert_version)
         self.response.headers['Location'] = hdr
         response_end( self, 302, "Location: %s" % hdr, "text/plain" )
         return
      
      # generate the certificate
      gateway_cert = ms_pb2.ms_gateway_cert()
      
      volume.protobuf_gateway_cert( gateway_cert, gateway )
      
      data = gateway_cert.SerializeToString()
      
      response_end( self, 200, data, "application/octet-stream" )
      return
   
      
      
class MSRegisterRequestHandler( GAEOpenIDRequestHandler ):
   """
   Generate a session certificate from a SyndicateUser account for a gateway.
   """

   OPENID_RP_REDIRECT_METHOD = "POST"     # POST to us for authentication, since we need to send the public key (which doesn't fit into a GET)

   def load_objects( self, gateway_type_str, gateway_name, username ):

      # get the gateway
      gateway = None
      if gateway_type_str == "UG":
         gateway = storage.get_user_gateway_by_name( gateway_name )
      elif gateway_type_str == "RG":
         gateway = storage.get_replica_gateway_by_name( gateway_name )
      elif gateway_type_str == "AG":
         gateway = storage.get_acquisition_gateway_by_name( gateway_name )
      else:
         logging.error("Invalid gateway type '%s'" % gateway_type_str )
         response_user_error( self, 401 )
         return (None, None)
         
      if gateway == None:
         logging.error("No such %s named %s" % (gateway_type_str, gateway_name))
         response_user_error( self, 404 )
         return (None, None)

      user = storage.read_user( username )
      if user == None:
         logging.error("storage.read_user returned None")
         response_user_error( self, 401 )
         return (None, None)

      return (gateway, user)

      
   def protobuf_volume( self, volume_metadata, volume, root=None ):
      if root != None:
         root.protobuf( volume_metadata.root )
         
      volume.protobuf( volume_metadata )

      return
      
      
   get = None
   
   def post( self, gateway_type_str, gateway_name, username, operation ):
      self.load_query()
      session = self.getSession()
      self.setSessionCookie(session)

      gateway, user = self.load_objects( gateway_type_str, gateway_name, username )

      if gateway == None or user == None:
         return

      # this SyndicateUser must own this Gateway
      if user.owner_id != gateway.owner_id:
         response_user_error( self, 403 )
         return

      if operation == "begin":

         # load the public key
         if not "syndicatepubkey" in self.request.POST:
            response_user_error( self, 400 )
            return

         pubkey = self.request.POST.get("syndicatepubkey")
         if pubkey == None:
            response_user_error( self, 400 )
            return
         
         # begin the OpenID authentication
         try:
            oid_request, rc = self.begin_openid_auth()
         except consumer.DiscoveryFailure, exc:

            fetch_error_string = 'Error in discovery: %s' % (cgi.escape(str(exc[0])))

            response_server_error( self, 500, fetch_error_string )
            return

         if rc != 0:
            response_server_error( self, 500, "OpenID error %s" % rc )
            return

         # preserve the public key
         session['syndicatepubkey'] = pubkey

         # reply with the redirect URL
         trust_root = OPENID_HOST_URL
         return_to = self.buildURL( "/REGISTER/%s/%s/%s/complete" % (gateway_type_str, gateway_name, username) )
         immediate = self.IMMEDIATE_MODE in self.query

         redirect_url = oid_request.redirectURL( trust_root, return_to, immediate=immediate )

         openid_reply = ms_pb2.ms_openid_provider_reply()
         openid_reply.redirect_url = redirect_url
         openid_reply.auth_handler = OPENID_PROVIDER_AUTH_HANDLER
         openid_reply.username_field = OPENID_PROVIDER_USERNAME_FIELD
         openid_reply.password_field = OPENID_PROVIDER_PASSWORD_FIELD
         openid_reply.extra_args = urllib.urlencode( OPENID_PROVIDER_EXTRA_ARGS )
         openid_reply.challenge_method = OPENID_PROVIDER_CHALLENGE_METHOD
         openid_reply.response_method = OPENID_PROVIDER_RESPONSE_METHOD
         openid_reply.redirect_method = self.OPENID_RP_REDIRECT_METHOD
         
         data = openid_reply.SerializeToString()

         session.save()
         
         response_end( self, 200, data, "application/octet-stream", None )
         return

      elif operation == "complete":

         # get our saved pubkey
         pubkey = session.get('syndicatepubkey')
         if pubkey == None:
            logging.error("could not load public key")
            response_user_error( self, 400 )
            return

         # complete the authentication
         info, _, _ = self.complete_openid_auth()
         if info.status != consumer.SUCCESS:
            # failed
            response_user_error( self, 401 )
            return
         
         new_cert = True
         
         # attempt to load it into the gateway
         rc = gateway.load_pubkey( pubkey )
         if rc != 0 and rc != -errno.EEXIST:
            logging.error("invalid public key (rc = %d)" % rc)
            response_user_error( self, 400 )
            return
         
         if rc == -errno.EEXIST:
            # no need to update the certificate
            new_cert = False
            
         # generate a session password
         session_pass = gateway.regenerate_session_password()
         gateway_fut = gateway.put_async()
         futs = [gateway_fut]

         registration_metadata = ms_pb2.ms_registration_metadata()

         # registration information
         registration_metadata.session_password = session_pass
         registration_metadata.session_expires = gateway.session_expires
         gateway.protobuf_cert( registration_metadata.cert )
         
         # find all Volumes
         volume = storage.get_volume( gateway.volume_id )
         
         if volume == None:
            response_user_error( self, 404 )
            return 
         
         roots = storage.get_volume_roots( [volume] )
         
         if roots == None or len(roots) == 0:
            response_user_error( self, 404 )
            return

         if new_cert:
            # next cert version (NOTE: this version increment does not need to be atomic; the version just needs to increase)
            volume.cert_version += 1
            
            vol_fut = volume.put_async()
            futs.append( vol_fut )
         
         self.protobuf_volume( registration_metadata.volume, volume, roots[0] )

         data = registration_metadata.SerializeToString()

         # save the gateway
         storage.wait_futures( futs )
         
         gateway.FlushCache( gateway.g_id )
         volume.FlushCache( volume.volume_id )
         
         response_end( self, 200, data, "application/octet-stream", None )
         
         return


class MSFileReadHandler(webapp2.RequestHandler):

   """
   Volume file request handler.
   It will read and list metadata entries via GET.
   """

   def get( self, volume_id_str, file_id_str, file_version_str, write_nonce_str ):

      file_request_start = storagetypes.get_time()

      file_id = -1
      file_version = -1
      write_nonce = 0
      try:
         file_id = MSEntry.unserialize_id( int( file_id_str, 16 ) )
         file_version = int( file_version )
         write_nonce = int( write_nonce_str )
      except:
         response_end( self, 400, "BAD REQUEST", "text/plain", None )
         return
      
      gateway, volume, timing = response_begin( self, volume_id_str )
      if volume == None:
         return
      
      if volume.need_gateway_auth() and gateway == None:
         response_user_error( self, 403 )
         return

      # this must be a User Gateway, if there is a specific gateway
      if gateway != None and not isinstance( gateway, UserGateway ):
         response_user_error( self, 403 )
         return
      
      # this gateway must have METADATA_READ caps
      if gateway != None and not gateway.check_caps( GATEWAY_CAP_READ_METADATA ):
         response_user_error( self, 403 )
         return 

      logging.info("resolve /%s/%s" % (volume.volume_id, file_id) )
      
      # request a file or directory
      resolve_start = storagetypes.get_time()

      owner_id = volume.owner_id
      if gateway != None:
         owner_id = gateway.owner_id
         
      reply = Resolve( owner_id, volume, file_id, file_version, write_nonce )
      
      resolve_time = storagetypes.get_time() - resolve_start
      
      logging.info("resolve /%s/%s rc = %d" % (volume.volume_id, file_id, reply.error) )

      timing['X-Resolve-Time'] = str(resolve_time)

      data = reply.SerializeToString()

      response_end( self, 200, data, "application/octet-stream", timing )
      return
      

class MSFileWriteHandler(webapp2.RequestHandler):
   
   """
   Volume file request handler.
   It will create, delete, and update metadata entries via POST.
   """
   
   @storagetypes.toplevel
   def post(self, volume_id_str ):

      file_post_start = storagetypes.get_time()
      
      # will have gotten metadata updates
      updates_field = self.request.POST.get( 'ms-metadata-updates' )

      if updates_field == None:
         # no valid data given (malformed)
         self.response.status = 202
         self.response.headers['Content-Type'] = "text/plain"
         self.response.write("%s\n" % -errno.EINVAL)
         return

      # extract the data
      data = updates_field.file.read()
      
      # parse it 
      updates_set = ms_pb2.ms_updates()

      try:
         updates_set.ParseFromString( data )
      except:
         self.response.status = 202
         self.response.headers['Content-Type'] = "text/plain"
         self.response.write("%s\n" % -errno.EINVAL)
         return

      # begin the response
      gateway, volume, timing = response_begin( self, volume_id_str )
      if volume == None:
         return

      # gateway must be known
      if gateway == None:
         response_user_error( self, 403 )
         return 
      
      # this can only be a User Gateway or an Acquisition Gateway
      if not isinstance( gateway, UserGateway ) and not isinstance( gateway, AcquisitionGateway ):
         response_user_error( self, 403 )
         return
      
      # if this is an archive, on an AG owned by the same person as the Volume can write to it
      if volume.archive:
         if (not isinstance( gateway, AcquisitionGateway ) or (isinstance( gateway, AcquisitionGateway and gateway.owner_id != volume.owner_id)) ):
            response_user_error( self, 403 )
            return 
      
      # if this is not an archive, then the gateway must have CAP_WRITE_METADATA
      elif not gateway.check_caps( GATEWAY_CAP_WRITE_METADATA ):
         response_user_error( self, 403 )
         return
       

      # validate the message
      if not gateway.verify_ms_update( updates_set ):
         # authentication failure
         self.response.status = 401
         self.response.headers['Content-Type'] = "text/plain"
         self.response.write( "Signature validation failed\n" )
         return

      # benchmarking information...
      create_times = []
      update_times = []
      delete_times = []
      chown_times = []
      rename_times = []

      reply = MS.methods.common.make_ms_reply( volume, 0 )
      reply.listing.status = 0
      reply.listing.ftype = 0
      reply.error = 0
      status = 200
      sign = False
      
      # validate operations
      for update in updates_set.updates:
         
         # valid operation?
         if update.type not in [ms_pb2.ms_update.CREATE, ms_pb2.ms_update.UPDATE, ms_pb2.ms_update.DELETE, ms_pb2.ms_update.CHOWN, ms_pb2.ms_update.RENAME]:
            # nope
            response_end( self, 501, "Method not supported", "text/plain", None )
            return
         
         # if the gateway sent a CHOWN request, it must have that capability
         if update.type == ms_pb2.ms_update.CHOWN and not gateway.check_caps( GATEWAY_CAP_COORDINATE ):
            response_user_error( self, 403 )
            return
         
         # if the operation is a RENAME request, then we must have two entries
         if update.type == ms_pb2.ms_update.RENAME and not update.HasField("dest"):
            logging.error("Update is for RENAME, but has no 'dest' field")
            response_user_error( self, 400 )
            return
         
      
      # carry out the operation(s)
      for update in updates_set.updates:

         # extract
         attrs = MSEntry.unprotobuf_dict( update.entry )

         rc = 0
         ent = None

         # create?
         if update.type == ms_pb2.ms_update.CREATE:
            logging.info("create /%s/%s (%s)" % (attrs['volume_id'], attrs['file_id'], attrs['name'] ) )
            
            create_start = storagetypes.get_time()
            
            rc, ent = MSEntry.Create( gateway.owner_id, volume, **attrs )
            
            create_time = storagetypes.get_time() - create_start
            create_times.append( create_time )
            
            # giving back information, so need to sign
            sign = True
            
            logging.info("create /%s/%s (%s) rc = %s" % (attrs['volume_id'], attrs['file_id'], attrs['name'], rc ) )
            
               
         # update?
         elif update.type == ms_pb2.ms_update.UPDATE:
            logging.info("update /%s/%s (%s)" % (attrs['volume_id'], attrs['file_id'], attrs['name'] ) )
            
            update_start = storagetypes.get_time()
            
            rc, ent = MSEntry.Update( gateway.owner_id, volume, **attrs )
            
            update_time = storagetypes.get_time() - update_start
            update_times.append( update_time )
            
            logging.info("update /%s/%s (%s) rc = %s" % (attrs['volume_id'], attrs['file_id'], attrs['name'], rc ) )

         # delete?
         elif update.type == ms_pb2.ms_update.DELETE:
            logging.info("delete /%s/%s (%s)" % (attrs['volume_id'], attrs['file_id'], attrs['name'] ) )
            
            delete_start = storagetypes.get_time()
            
            rc = MSEntry.Delete( gateway.owner_id, volume, **attrs )
            ent = None
            
            delete_time = storagetypes.get_time() - delete_start
            delete_times.append( delete_time )
            
            logging.info("delete /%s/%s (%s) rc = %s" % (attrs['volume_id'], attrs['file_id'], attrs['name'], rc ) )

         # change coordinator? 
         elif update.type == ms_pb2.ms_update.CHOWN:
            
            logging.info("chcoord /%s/%s (%s) to %s" % (attrs['volume_id'], attrs['file_id'], attrs['name'], gateway.g_id) )
            
            chown_start = storagetypes.get_time()
            
            rc, ent = MSEntry.Chcoord( gateway.owner_id, volume, gateway, **attrs )
            
            chown_time = storagetypes.get_time() - chown_start
            chown_times.append( chown_time )
            
            # giving back information, so need to sign
            sign = True 
            
            logging.info("chcoord /%s/%s (%s) rc = %d" % (attrs['volume_id'], attrs['file_id'], attrs['name'], rc ) )
            
         # rename?
         elif update.type == ms_pb2.ms_update.RENAME:
            src_attrs = attrs
            dest_attrs = MSEntry.unprotobuf_dict( update.dest )
            
            logging.info("rename /%s/%s (name=%s, parent=%s) to (name=%s, parent=%s)" % 
                         (src_attrs['volume_id'], src_attrs['file_id'], src_attrs['name'], src_attrs['parent_id'], dest_attrs['name'], dest_attrs['parent_id']) )
            
            rename_start = storagetypes.get_time()
            
            rc = MSEntry.Rename( gateway.owner_id, volume, src_attrs, dest_attrs )
            ent = None
            
            rename_time = storagetypes.get_time() - rename_start
            rename_times.append( rename_time )
            
            logging.info("rename /%s/%s (name=%s, parent=%s) to (name=%s, parent=%s) rc = %s" % 
                         (src_attrs['volume_id'], src_attrs['file_id'], src_attrs['name'], src_attrs['parent_id'], dest_attrs['name'], dest_attrs['parent_id'], rc) )
            
         else:
            # not a valid method (shouldn't ever reach here...)
            response_end( self, 501, "Method not supported", "text/plain", None )
            return
         
         if rc >= 0 and ent != None:
            # success
            ent_pb = reply.listing.entries.add()
            ent.protobuf( ent_pb )
         elif rc < 0:
            # error
            reply.error = rc
            break


      if len(create_times) > 0:
         timing['X-Create-Times'] = ",".join( [str(t) for t in create_times] )

      if len(update_times) > 0:
         timing['X-Update-Times'] = ",".join( [str(t) for t in update_times] )

      if len(delete_times) > 0:
         timing['X-Delete-Times'] = ",".join( [str(t) for t in delete_times] )

      if len(chown_times) > 0:
         timing['X-Chcoord-Times'] = ",".join( [str(t) for t in chown_times] )
         
      if len(rename_times) > 0:
         timing['X-Rename-Times'] = ",".join( [str(t) for t in rename_times] )
         
      # sign the response
      reply.signature = ""

      reply_str = reply.SerializeToString()
      
      if sign:
         sig = volume.sign_message( reply_str )

         reply.signature = sig

         reply_str = reply.SerializeToString()
         
      response_end( self, status, reply_str, "application/octet-stream", timing )
         
      return


class MSOpenIDRequestHandler(GAEOpenIDRequestHandler):

   def auth_redirect( self, **kwargs ):
      """
      What to do if the user is already authenticated
      """
      session = self.getSession()
      if 'login_email' not in session:
         # invalid session
         response_user_error( self, 400, "Invalid or missing session cookie" )
         return 

      self.setRedirect('/syn/')
      return 0
      

   def verify_success( self, request, openid_url ):
      session = self.getSession()
      session['login_email'] = self.query.get('openid_username')
      return 0


   def process_success( self, info, sreg_resp, pape_resp ):
      self.auth_redirect()
      return 0
      

class MSJSONRPCHandler(webapp2.RequestHandler):
   """
   JSON RPC request handler 
   """
   
   def post( self ):
      # pass on to JSON RPC server
      server = rpc.jsonrpc.Server( api.API( self.request.headers ) )
      server.handle( self.request, self.response )
   
   def get( self ):
      # pass on to JSON RPC server
      server = rpc.jsonrpc.Server( api.API( self.request.headers ) )
      server.handle( self.request, self.response )
   
