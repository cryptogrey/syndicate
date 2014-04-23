#!/usr/bin/python

import os
import sys
import traceback
import base64

if __name__ == "__main__":
    # for testing 
    if os.getenv("OPENCLOUD_PYTHONPATH"):
        sys.path.append( os.getenv("OPENCLOUD_PYTHONPATH") )
    else:
        print >> sys.stderr, "No OPENCLOUD_PYTHONPATH variable set.  Assuming that OpenCloud is in PYTHONPATH"
 
    os.environ.setdefault("DJANGO_SETTINGS_MODULE", "planetstack.settings")


from django.db.models import F, Q
from planetstack.config import Config
from observer.syncstep import SyncStep
from core.models import Service
from syndicate.models import Volume
from util.logger import Logger, logging
logger = Logger(level=logging.INFO)

# syndicatelib will be in stes/..
parentdir = os.path.join(os.path.dirname(__file__),"..")
sys.path.insert(0,parentdir)

import syndicatelib


class SyncVolume(SyncStep):
    provides=[Volume]
    requested_interval=0

    def __init__(self, **args):
        SyncStep.__init__(self, **args)

    def fetch_pending(self):
        try:
            ret = Volume.objects.filter(Q(enacted__lt=F('updated')) | Q(enacted=None))
            return ret
        except Exception, e:
            traceback.print_exc()
            return None

    def sync_record(self, volume):
        try:
            print "\n\nSync!"
            print "volume = %s\n\n" % volume.name
        
            user_email = volume.owner_id.email
            config = syndicatelib.get_config()

            # get the observer secret 
            try:
                observer_secret = config.SYNDICATE_OPENCLOUD_SECRET
            except Exception, e:
                traceback.print_exc()
                logger.error("config is missing SYNDICATE_OPENCLOUD_SECRET")
                return False

            # owner must exist...
            try:
                new_user = syndicatelib.ensure_user_exists( user_email )
            except Exception, e:
                traceback.print_exc()
                logger.error("Failed to ensure user '%s' exists" % user_email )
                return False

            # if we created a new user, then save its credentials 
            if new_user is not None:
                try:
                    rc = syndicatelib.save_syndicate_principal( user_email, observer_secret, new_user['signing_public_key'], new_user['signing_private_key'] )
                    assert rc == True, "Failed to save SyndicatePrincipal"
                except Exception, e:
                    traceback.print_exc()
                    logger.error("Failed to save private key for principal %s" % (user_email))
                    return False

            print "\n\nuser for %s: %s\n\n" % (user_email, new_user)

            # volume must exist 
            try:
                new_volume = syndicatelib.ensure_volume_exists( user_email, volume, user=new_user )
                syndicatelib.ensure_volume_exists( user_email, volume, user=new_user )
            except Exception, e:
                traceback.print_exc()
                logger.error("Failed to ensure volume '%s' exists" % volume.name )
                return False

            print "\n\nvolume for %s: %s\n\n" % (volume.name, new_volume)
            
            # did we create the Volume?  If so, set up its slice
            if new_volume is not None:
                pass

            # otherwise, just update it 
            else:
                try:
                    rc = syndicatelib.update_volume( volume )
                except Exception, e:
                    traceback.print_exc()
                    logger.error("Failed to update volume '%s', exception = %s" % (volume.name, e.message))
                    return False
                    
                return True
                
            return True

        except Exception, e:
            traceback.print_exc()
            return False
        



if __name__ == "__main__":
    sv = SyncVolume()


    # first, set all volumes to not-enacted so we can test 
    for v in Volume.objects.all():
       v.enacted = None
       v.save()
    
    recs = sv.fetch_pending()

    for rec in recs:
        rc = sv.sync_record( rec )
        if not rc:
          print "\n\nFailed to sync %s\n\n" % (rec.name)

