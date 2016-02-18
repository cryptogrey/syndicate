Syndicate
=========

Syndicate is a **scalable software-defined storage system for wide-area networks**.   Syndicate creates global read/write storage volumes on top of existing systems, but while preserving end-to-end domain-specific storage invariants.  With less than 200 lines of Python, and without having to run any servers of your own, you use a Syndicate volume to do things like:
* Scale up reads on a remote server with a CDN while guaranteeing that readers always see fresh data.
* Turn a collection of URLs as a copy-on-write shared filesystem which preserves your and your peers' changes.
* Implement end-to-end encryption on top of your Dropbox folder, while mirroring your files to Amazon S3 and Google Drive.
* Publish your code for the world to download, while guaranteeing end-to-end authenticity and integrity.

Examples
--------

Here are a few examples of how we are currently using Syndicate:

* Augmenting scientific storage systems (like [iRODS](https://irods.org)) and public datasets (like [GenBank](https://www.ncbi.nlm.nih.gov/genbank/)) with ingress Web caches in order to automatically stage large-scale datasets for local compute clusters to process. 
* Creating a secure [DropBox](http://www.dropbox.com)-like "shared folder" system for [OpenCloud](http://www.opencloud.us) that augments VMs, external scientific datasets, and personal computers with a private CDN, allowing users to share large amounts of data with their VMs while minimizing redundant transfers.
* Scalably packaging up and deploying applications across the Internet.
* Creating webmail with transparent end-to-end encryption, automatic key management, and backwards compatibility with email.  Email data gets stored encrypted to user-chosen storage service(s), so webmail providers like [Gmail](https://mail.google.com) can't snoop.  See the [SyndicateMail](https://github.com/jcnelson/syndicatemail) project for details.

The "secret sauce" is a novel programming model that lets the application break down storage I/O logic into a set of small, mostly-orthogonal but composible I/O steps.  By combining these steps into a networked pipeline and controlling when and where each step can execute in the network, the application can preserve domain-specific invariants end-to-end without having to build a whole storage abstraction layer from scratch.

Where can I learn more?
-----------------------

Please see a look at our [whitepaper](https://www.cs.princeton.edu/~jcnelson/acm-bigsystem2014.pdf), published in the proceedings of the 1st International Workshop on Software-defined Ecosystems (colocated with HPDC 2014).

Also, please see [our NSF grant](http://www.nsf.gov/awardsearch/showAward?AWD_ID=1541318&HistoricalAwards=false) for our ongoing work.

Building
--------

To build, type:
```
    $ make MS_APP_ADMIN_EMAIL=(your admin account email)
```

**NOTE:**  At this time, there are no `install` targets for the executables (this will be added soon).  For now, executables are put into directories within `./build/out/bin`.

To build Syndicate, you will need the following tools, libraries, and header files:
* [libcurl](http://curl.haxx.se/libcurl/)
* [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/)
* [Google Protocol Buffers](https://github.com/google/protobuf)
* [OpenSSL](https://www.openssl.org/)
* [libjson](https://github.com/json-c/json-c)
* [fskit](https://github.com/jcnelson/fskit)
* [libfuse](https://github.com/libfuse/)
* [Python 2.x](https://www.python.org)
* [Cython](https://github.com/cython/cython)
* [boto](https://github.com/boto/boto)


Quick Setup
-----------

Syndicate's end-point components (**gateways**) coordinate via an untrusted logically central **metadata service** (MS).  The MS implementation currently runs in Google AppEngine, or in [AppScale](https://github.com/AppScale/appscale).  You can test it locally with the [Python GAE development environment](https://cloud.google.com/appengine/downloads?hl=en).  Please see the relevant documentation for GAE, AppScale, and the development environment for deployment instructions.  You should be able to run the MS on the free tier in GAE.

The MS code is built to `./build/out/ms`.  You can deploy it to GAE from there, or run it with the development environment with:
```
    $ dev_appserver.py ./build/out/ms
```

Now, you need to set up your `~/.syndicate` directory.  An admin account and keypair are automatically generated by the build process (in `./build/out/ms`).  To use them, type:
```
    $ cd ./build/out/bin/
    $ ./syndicate setup $MS_APP_ADMIN_EMAIL ../ms/admin.pem http://localhost:8080
```

Replace `$MS_APP_ADMIN_EMAIL` from the value you passed to `make` earlier, and replace `http://localhost:8080` with the URL to your MS deployment (if you're not running the `dev_appserver.py`).

First, you must create a volume.  Here's an example that will create a volume called `test-volume`, owned by the administrator, with a 64k block size:

```
    $ ./syndicate create_volume name="test-volume" description="This is a description of the volume" blocksize=65536 email="$MS_APP_ADMIN_EMAIL"
```

Now, you can create gateways for the volume--the network-addressed processes that bind existing storage systems together and overlay your application's domain-specific I/O coordination logic over them.  To create a **user gateway** that will allow you to interact with the `test-volume` volume's data via the admin account, type:

```
    $ ./syndicate create_gateway email="$MS_APP_ADMIN_EMAIL" volume="test-volume" name="test-volume-UG-01" private_key=auto type=UG
    $ ./syndicate update_gateway test-volume-UG-01 caps=ALL
```

This will create a user gateway called `test-volume-UG-01`, generate and store a key pair for it automatically, and enable all capabilities for it.  Similarly, you can do so for **replica gateways** (using `type=RG`) and **acquisition gateways** (using `type=AG`).  Replica gateways take data from user gateways and forward it to persistent storage.  Acquisition gateways consume data from existing storage and make it available as files and directories in the volume.

Gateways are dynamically programmable via drivers.  There are some sample drivers in `python/syndicate/drivers/ag/disk` (an AG driver that imports local files into a volume) and `python/syndicate/drivers/rg/s3` (an RG driver that replicates data to Amazon S3).  Drivers are specific to the type of gateway--you cannot use an RG driver for an AG, for example.

To set a driver, you should use the `driver=` flag in the `update_gateway` directive.  For example:

```
    $ ./syndicate update_gateway sample-AG driver=../python/syndicate/drivers/ag/disk
```

The gateway should automatically fetch and instantiate the driver when it next reloads and revalidates its certificate bundle.
