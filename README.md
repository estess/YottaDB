# YottaDB

All software in this package is part of YottaDB (http://yottadb.com) each
file of which identifies its copyright holders. The software is made available
to you under the terms of a license. Refer to the [LICENSE](LICENSE) file for details.

YottaDB relies on CMake to generate the Makefiles to build binaries from source.
The prerequisites are CMake (at least 2.8.5), GNU make (at least 3.81), Linux
(`x86_64`), libraries and development files for libz, Unicode, OpenSSL and GPG.
Ubuntu 16.04 LTS was used to test the builds for this distribution, with default
versions of packages from the distribution repositories.

## How to build

1. Fulfill the pre-requisites

   Install developement libraries

   ```sh
    cmake tcsh {libconfig,libelf,libgcrypt,libgpg-error,libgpgme11,libicu,libncurses,libssl,zlib1g}-dev
   ```

   There may be other library dependencies or the packages may have different names.
   If CMake issues a NOTFOUND error, please see the FAQ below.

2. Unpack the YottaDB sources

   The YottaDB source tarball extracts to a directory with the version number in the name, i.e. ```yottadb_r110```

   ```sh
   $ tar xfz yottadb_r110_src.tar.gz
   $ cd yottadb_r110
   ```

   You should find this README, LICENSE, COPYING and CMakeLists.txt file and sr_* source directories.

3. Build the YottaDB binaries

   ```sh
   $ mkdir build
   $ cd build
   ```

   > By default the script creates production (pro) builds of YottaDB. To create
   > a debug (dbg) build of YottaDB supply the following parameter to cmake
   >     ```-D CMAKE_BUILD_TYPE=Debug```	*Note: title case is important*
   >

   ```sh
   $ cmake ../
   $ make
   $ make install
   $ cd yottadb/r110
   ```

  Note that the make install done above does not create the final installed YottaDB.
  Instead, it stages YottaDB for distribution.

  Now you are ready to install YottaDB. Answer a few questions and install it.
  The recommended installation path is `/opt/yottadb/r110`

  ```sh
  $ sudo ./ydbinstall	# to install the version you just built
  $ cd - ; make clean
  ```

  (Note: The below will help get installation options if needed.)

  ```sh
  # ./ydbinstall --help	# get installation options
  ```

4. Packaging YottaDB

   Create a tar file from the installed directory

## FAQ
- The CMake build fails with the following message followed by one or more cases.
	CMake Error: The following variables are used in this project, but they are set to NOTFOUND.
	Please set them or make sure they are set and tested correctly in the CMake files:
  This indicates that required libraries are not found. Please consult the list
  of libraries and check your distributions package manager.
