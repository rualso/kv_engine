введение по сборке couchbase 3.0.3.teligent ветки под RHEL5 64 bit
==================================================================

все действия от root.

создать виртуалку (virtualbox) 
------------------------------

из образа
http://autobuild.teligent.ru/kickstarts/isos/RHEL/5/rhel-server-5.11-x86_64-dvd.iso
при установке выбрал пункт "Software development workstation".

установить пакеты
-----------------

~~~
rpm -ihv http://autobuild.teligent.ru/kickstarts/3RD_PARTY/epel/5/x86_64/cmake28-2.8.11.2-2.el5.x86_64.rpm http://autobuild.teligent.ru/kickstarts/3RD_PARTY/epel/5/x86_64/libarchive-2.8.4-6.el5.x86_64.rpm
ln -sf /usr/bin/cmake28 /usr/bin/cmake
~~~

установить couchbase-server-3.0.1 и выкачать исходники
------------------------------------------------------
[см](README-TELIGENT.md)

запустить общую сборку, среди прочего получится ep.so
-----------------------------------------------------

~~~
vim memcached/CMakeLists.txt
#CHECK_INCLUDE_FILE_CXX("atomic" HAVE_ATOMIC)

#начните собирать, оно сломается, поправьте ниже и ещё раз сборка (не стал вникать)
vim build/CMakeCache.txt
#исправьте эти на такие
HAVE_ARPA_INET_H:INTERNAL=1
HAVE_INTTYPES_H:INTERNAL=1

#это найдёт v8 в /opt/couchbase
#и указываем на уже собранное:
V8_DIR=$PWD/v8 make PREFIX=/opt/couchbase CMAKE_PREFIX_PATH=/opt/couchbase EXTRA_CMAKE_OPTIONS='-D CMAKE_BUILD_TYPE=RelWithDebInfo'
...
-- Installing: /opt/couchbase/lib/memcached/ep.so
-- Set runtime path of "/opt/couchbase/lib/memcached/ep.so" to "$ORIGIN/../lib:$ORIGIN/../lib/memcached:/opt/couchbase/lib:/opt/couchbase/lib/memcached:/opt/couchbase/lib"
[root@rualpe-vm2 couchbase]#
~~~

выложить результат
==================

[см](README-TELIGENT.md)
