﻿Davrods - An Apache WebDAV interface to iROD
=============================================

Davrods provides access to iRODS servers using the WebDAV protocol.
It is a bridge between the WebDAV protocol and the iRODS API,
implemented as an Apache HTTPD module.

Davrods leverages the Apache server implementation of the WebDAV
protocol, `mod_dav`, for compliance with the WebDAV Class 2 standard.

Notable features include:

- Supports WebDAV Class 2. Locks are local to the Apache server.
- Supports PAM and Native (a.k.a. STANDARD) iRODS authentication.
- Supports SSL encryption for the entire iRODS connection.
- Easy to configure using Apache configuration directives.
- Supports iRODS server versions 4+ and is backwards compatible with 3.3.1.
- Themeable listings similar to using mod_autoindex.
- Expose and navigate by metadata key-value pairs.
- Search the metadata catalogue.
- REST API for accessing and manipulating the iRODS metadata 
- Client-side interface to add, edit and delete metadata entries.

A working demo of this module showing the themeable listings along with metadata 
searching and linking is available at https://wheatis.tgac.ac.uk/davrods/browse/reads/.


## Installation ##

### Prerequisites ###

Davrods requires the following packages to be installed on your server:

- Apache httpd 2.4+
- iRODS 4.x client libraries (in package `irods-runtime`, available
  from [the iRODS website](http://irods.org/download/))

Due to the way iRODS libraries are packaged, specifically, its network
plugins, one of the following packages must also be installed:

- `irods-icommands` *OR* `irods-icat` *OR* `irods-resource`.

These three packages all provide the necessary libraries in
`/var/lib/irods/plugins/network`.

### Using the binary distribution ###

For binary installation, download the package for your platform at
https://github.com/billyfish/davrods/releases and copy the module to the 
`modules` directory of your Apache httpd installation. If the binary version 
is not available, then you can compile the module from source.


### Compiling 

To compile Davrods from source, copy `example-user.prefs` to `user.prefs` and edit 
it to contain values valid for your system. The file is commented and should be able to 
to be set up with a minimum of effort.

Once this is complete, then ```make``` followed by ```make install``` will create 
and install `mod_davrods.so` to your Apache httpd installation.


See the __Configuration__ section for instructions on how to configure
Davrods once it has been installed.

### Davrods and SELinux ##

If the machine on which you install Davrods is protected by SELinux,
you may need to make changes to your policies to allow davrods to run:

- Apache HTTPD must be allowed to connect to TCP port 1247
- Davrods must be allowed to dynamically load iRODS client plugin
  libraries in /var/lib/irods/plugins/network

For example, the following two commands can be used to resolve these
requirements:

    setsebool -P httpd_can_network_connect true
    chcon -t lib_t /var/lib/irods/plugins/network/lib*.so

## Configuration ##

Davrods is configured in two locations: In a HTTPD vhost configuration
file and in an iRODS environment file. The vhost config is the main
configuration file, the iRODS environment file is used for iRODS
client library configuration, similar to the configuration of
icommands.


### HTTPD vhost configuration ###

The Davrods RPM distribution installs a commented out vhost template
in `/etc/httpd/conf.d/davrods-vhost.conf`. With the comment marks
(`#`) removed, this provides you with a sane default configuration
that you can tune to your needs. The Davrods configuration
options are documented in this file and can be changed to your liking.
As well as that documentation, the new features are also described
below.

#### Public access

If you wish to run Davrods without the need for authentication, it 
can be set up to run as a normal public-facing website. This is done by
specifying a user name and password for the iRODS user whose data you 
wish to display. The directives for these are `DavRodsDefaultUsername`
and `DavRodsDefaultPassword`.

For example, to display the data for a user with the 
user name *anonymous* and password *foobar* the configuration directives
would be:

```
DavRodsDefaultUsername anonymous
DavRodsDefaultPassword foobar
```

#### Themed Listings

By default, the html listings generated by mod_davrods do not use any 
styling. It is possible to style the listings much like [mod_autoindex](https://httpd.apache.org/docs/2.4/mod/mod_autoindex.html). There are
various directives that can be used.

* **DavRodsThemedListings**:
This directive is a global on/off switch which toggles the usage of 
themed listings. Unless this is set to *true* then none of the following
directives in this section will work. To turn it on, use the following 
directive:

 ```
 DavrodsThemedListings true
 ```

* **DavRodsHTMLHead**:
This is an HTML text that will be placed within the *\<head\>* directive
of the HTML pages generated by Davrods. This can either be set as a string 
for this directive in which case, as it is a single-line directive, if you 
want to define this across multiple lines of text you will need a backslash 
(\\) at the end of each line. 

 ```
 DavRodsHTMLHead <link rel="stylesheet" type="text/css"\ 
 media="all" href="/davrods_files/styles/styles.css">
 ```


It also can point at a file whose content will be used for the required HTML
chunk by beginning your value with "file:", *e.g.* to use the content of a file
at /opt/apache/davrods_head.html

 ```
 DavRodsHTMLTop file:/opt/apache/davrods_head.html
 ```

If a file is used, then it will be re-read for each incoming request. So any 
changes you make to the file won't need a restart of Apache to be made live.

* **DavRodsHTMLTop**:
You can specify a chunk of HTML to appear above each listing using this 
directive. This can either be set as a string for this directive in which 
case, as it is a single-line directive, if you want to define this 
across multiple lines of text you will need a backslash (\\) at the end of each 
line. For instance to have a *\<header\>* section containing a logo
on each page, you could use the following directive:

 ```
 DavRodsHTMLTop <header><img src="logo.png" alt="Company logo" /> \
 	</header>
 ```

It also can point at a file whose content will be used for the required HTML
chunk by beginning your value with "file:", *e.g.* to use the content of a file
at /opt/apache/davrods_top.html

 ```
 DavRodsHTMLTop file:/opt/apache/davrods_top.html
 ```

If a file is used, then it will be re-read for each incoming request. So any 
changes you make to the file won't need a restart of Apache to be made live.

* **DavRodsHTMLBottom**:
You can specify a chunk of HTML to appear above each listing using this 
directive. This can either be set as a string for this directive in which 
case, as it is a single-line directive, if you want to define this 
across multiple lines of text you will need a backslash (\\) at the end of each 
line. For instance to have a *\<header\>* section containing a logo
on each page, you could use the following directive:

 ```
 DavRodsHTMLBottom <footer>Listing generated by mod_davrods, \
  &copy; 2016 by Utrecht University and &copy; 2017 by the \
  Earlham Institute.<br /> Filetype icons are taken from the \
  Amiga Image Storage System &copy; 2004 - 2016 by Martin \
  Mason Merz.</footer>
 ```

It also can point at a file whose content will be used for the required HTML
chunk by beginning your value with "file:", *e.g.* to use the content of a file
at /opt/apache/davrods_bottom.html

 ```
 DavRodsHTMLTop file:/opt/apache/davrods_bottom.html
 ```

If a file is used, then it will be re-read for each incoming request. So any 
changes you make to the file won't need a restart of Apache to be made live.



* **DavRodsHTMLListingClass**:
The list of collections and data objects displayed by Davrods are within 
an HTML table. If you wish to specify CSS classes for this table, you can 
use this directive. For instance, if you wish to use the CSS styles 
called *table* and *table-striped*, then you could use the following 
directive:

 ```
 DavRodsHTMLListingClass table table-striped
 ```
 
* **DavRodsHTMLCollectionIcon**:
If you wish to use a custom image to denote collections, you can use this
directive. This can be superseded by a matching call to the `DavRodsAddIcon`
directive. For instance, to use */davrods_files/images/drawer* for all
collections, the directive  would be:

 ```
 DavRodsHTMLCollectionIcon /davrods_files/images/drawer
 ```
 
* **DavRodsHTMLObjectIcon**:
If you wish to use a custom image to denote data objects, you can use 
this directive. This can be superseded by a matching call to the 
`DavRodsAddIcon` directive. For instance, to use 
*/davrods_files/images/file* for all data objects, the directive would be:

 ```
 DavRodsHTMLObjectIcon /davrods_files/images/file
 ```

* **DavRodsHTMLParentIcon**:
This directive allows you to use a custom image to denote the link 
to the parent collection. For instance, to use 
*/davrods_files/images/parent*, the directive  would be:

 ```
 DavRodsHTMLParentIcon /davrods_files/images/parent
 ```

* **DavRodsAddIcon**:
This directive allows you to specify icons that will be displayed in the 
listing for matching collections and data objects. This directive takes 
two or more arguments the first of which is the path to the image file 
to use. The remaining arguments are the file suffices that will be 
matched and use the given icon. For example, to use an archive image for
various compressed files and a picture image for image files, the 
directives would be: 

 ```
 DavRodsAddIcon /davrods_files/images/archive .zip .tgz 
 DavRodsAddIcon /davrods_files/images/image .jpeg .jpg .png
 ```
 
#### Metadata

Each data object and collection can also display its metadata AVUs 
and have these as clickable links to allow a user to browse all 
data objects and collections with matching metadata too. 


* **DavRodsHTMLMetadata**:
This directive is toggles the usage of metadata within the listings. 
It can take one of the following values:

	* **full**: All of the metadata for each entry will get sent in the
HTML page for each request.
 * **on_demand**: None of the metadata is initially included with the 
HTML pages sent by Davrods. Instead they can be accessed via AJAX requests
from these pages.
  * **none**: No metadata information will be made available.
 So to set the metadata to be available on demand, the directive would be:

     ```
  DavRodsHTMLMetadata on_demand
     ```

* **DavRodsHTMLMetadataEditable**:
This directive specifies whether the client-side functionality for editing
the metadata by accessing REST API calls is active ot not. By default, it is
off and can be turned on by setting this directive to true.

 ```
 DavRodsHTMLMetadataEditable true
 ```


* **DavRodsAPIPath**:
This directive specifies the path used within Davrods to link to the
various REST API functionality such as the metadata search 
functionality. To specify it as */api/*, which is a good default, 
use the following directive:

 ```
 DavRodsAPIPath /api/
 ```

* **HTMLAddMetadataImage**:
If ```DavRodsHTMLMetadataEditable``` is set to true, then you can use this
directive to specify the image used for the button to add metadata to a 
particular data object or collection.

 ```
 DavRodsHTMLAddMetadataImage /davrods_files/images/list_add
 ```

* **HTMLDeleteMetadataImage**:
If ```DavRodsHTMLMetadataEditable``` is set to true, then you can use this
directive to specify the image used for the button to delete metadata from a 
particular data object or collection.

 ```
 DavRodsHTMLDeleteMetadataImage /davrods_files/images/list_delete
 ```

* **HTMLEditMetadataImage**:
If ```DavRodsHTMLMetadataEditable``` is set to true, then you can use this
directive to specify the image used for the button to edit metadata for a 
particular data object or collection.

 ```
 DavRodsHTMLEditMetadataImage /davrods_files/images/list_edit
 ```

* **DavRodsHTMLOkImage**:
If ```DavRodsHTMLMetadataEditable``` is set to true, then you can use this
directive to specify the image used for the "OK" button of the metadata 
editor.

 ```
 DavRodsHTMLOkImage /davrods_files/images/list_ok
 ```

* **DavRodsHTMLCancelImage**:
If ```DavRodsHTMLMetadataEditable``` is set to true, then you can use this
directive to specify the image used for the "Cancel" button of the metadata 
editor.

 ```
 DavRodsHTMLOkImage /davrods_files/images/list_cancel
 ```

#### REST API

Davrods has a REST API for accessing and manipulating the iRODS metadata catalog. 
Currently it has the following functions:

 * **metadata/search**:
 * **metadata/edit**:
 * **metadata/get**:
 * **metadata/add**:
 * **metadata/delete**:


### The iRODS environment file ###

The binary distribution installs the `irods_environment.json` file in
`/etc/httpd/irods`. In most iRODS setups, this file can be used as
is.

Importantly, the first seven options (from `irods_host` up to and
including `irods_zone_name`) are **not** read from this file. These
settings are taken from their equivalent Davrods configuration
directives in the vhost file instead.

The options in the provided environment file starting from
`irods_client_server_negotiation` *do* affect the behaviour of
Davrods. See the official documentation for help on these settings at:
https://docs.irods.org/master/manual/configuration/#irodsirods_environmentjson

For instance, if you want Davrods to connect to iRODS 3.3.1, the
`irods_client_server_negotiation` option must be set to `"none"`.


## Building from source ##

To build from source, the following build-time dependencies must be
installed (package names may differ on your platform):

- `httpd-devel >= 2.4`
- `apr-devel`
- `apr-util-devel`
- `irods-dev`

Additionally, the following runtime dependencies must be installed:

- `httpd >= 2.4`
- `irods-runtime >= 4.1.8`
- `jansson`
- `boost`
- `boost-system`
- `boost-filesystem`
- `boost-regex`
- `boost-thread`
- `boost-chrono`

First, browse to the directory where you have unpacked the Davrods
source distribution.

Running `make` without parameters will generate the Davrods module .so
file in the `.libs` directory. `make install` will install the module
into Apache's modules directory.

After installing the module, copy the `davrods.conf` file to
`/etc/httpd/conf.modules.d/01-davrods.conf`.

Note: Non-Redhat platforms may have a different convention for the
location of the above file and the method for enabling/disabling
modules, consult the respective documentation for details.

Create an `irods` folder in a location where Apache HTTPD has read
access (e.g. `/etc/httpd/irods`). Place the provided
`irods_environment.json` file in this directory. For most setups, this
file can be used as is (but please read the __Configuration__ section).

Finally, set up httpd to serve Davrods where you want it to. An
example vhost config is provided for your convenience.

## Tips and Tricks ##

If you are dealing with big files, you will almost certainly want to enable 
Apache's compression functionality using [mod_deflate](http://httpd.apache.org/docs/current/mod/mod_deflate.html).

For instance, to enable compressed response bodies from Apache, you might want a configuration such as

```
LoadModule deflate_module modules/mod_deflate.so
SetOutputFilter DEFLATE
SetEnvIfNoCase Request_URI "\.(?:gif|jpe?g|png|gzip|zip|bz2)$" no-gzip
```

## Bugs and ToDos ##

Please report any issues you encounter on the
[issues page](https://github.com/UtrechtUniversity/davrods/issues).

## Authors ##

[Chris Smeele](https://github.com/cjsmeele) and [Simon Tyrrell](https://github.com/billyfish).

## Contact information ##

For questions or support on the WebDAV functionality, contact Chris Smeele or Ton Smeele either
directly or via the
[Utrecht University RDM](http://www.uu.nl/en/research/research-data-management/contact-us)
page.
For questions or support on the themeable listings and metadata functionality, contact Simon Tyrrell
directly or via the [Earlham Institute](http://www.earlham.ac.uk/contact-us/) page.

## License ##

Copyright (c) 2016, Utrecht University  and (c) 2017, Earlham Institute.

Davrods is licensed under the GNU Lesser General Public License version
3 or higher (LGPLv3+). See the COPYING.LESSER file for details.

The `lock_local.c` file was adapted from the source of `mod_dav_lock`,
a component of Apache HTTPD, and is used with permission granted by
the Apache License. See the copyright and license notices in this file
for details.
