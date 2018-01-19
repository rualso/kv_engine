введение по сборке couchbase, teligent ветка
============================================

RHEL7
все действия от root

создать виртуалку (virtualbox) 
------------------------------

http://autobuild.teligent.ru/kickstarts/isos/RHEL/7/rhel-server-7.0-x86_64-dvd.iso
при установке выбрал пункт "Basic development workstation".

добавить в /etc/yum.repo.d файлик local64.repo
~~~
[root@rualpe-vm1 v8]# cat /etc/yum.repos.d/local64.repo 
[local64]
name=Red Hat Enterprise Linux  -  - Local64
baseurl=http://autobuild.teligent.ru/kickstarts/redhat/rhel/7.0/os/x86_64/
enabled=1
gpgcheck=0

[opt]
name=opt
baseurl=http://autobuild.teligent.ru/kickstarts/redhat/rhel/7/optional/x86_64/
enabled=1
gpgcheck=0
~~~


установить пакеты
-----------------

~~~
yum install cmake snappy-devel.x86_64 libicu-devel.x86_64  openssl-devel.x86_64 libevent-devel.x86_64

yum install asciidoc ntpdate http://autobuild.teligent.ru/kickstarts/mrepo/7Server-x86_64/updates/Packages/tzdata-2016f-1.el7.noarch.rpm ftp://rpmfind.net/linux/centos/7.3.1611/os/x86_64/Packages/cmake-2.8.12.2-2.el7.x86_64.rpm
cat>/etc/ntp.cfg
server 192.168.2.30
^D
service ntpdate restart

/usr/include/unicode/uvernum.h
поправить, чтобы был такой суффикс
#define U_ICU_VERSION_SUFFIX _44

~~~

сборка community vanilla версии в rpm
-------------------------------------

couchbase-server-enterprise-5.0.1-centos7.x86_64.rpm
поставить rpm и залить
scp /Downloads/couchbase-server-enterprise-5.0.1-centos7.x86_64.rpm alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/
#теперь доступно http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-enterprise-4.5.1-centos7.x86_64.rpm

mkdir -p ~/rpmbuild/SPECS
cd ~/rpmbuild/SPECS
#восстановть спеку
rpmrebuild -e couchbase-server-enterprise
:w couchbase-server-community-5.0.1.spec
#поправить couchbase-server на couchbase-server-community (одно место) и наоборот (одно место)

после
#SOURCERPM:    couchbase-server-5.0.1-5003.src.rpm
добавить
Source1: couchbase-server-enterprise-to-community-5.0.1-centos7.x86_64.tgz

добавить
%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/
cd $RPM_BUILD_ROOT
#rpm2cpio %{SOURCE0} | cpio -idmv
tar vxzf %{SOURCE1}

перед %files


scp couchbase-server-community-5.0.1.spec alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
#теперь доступно http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/SRPM/couchbase-server-community-5.0.1.spec

---

mkdir ~/couchbase.5.0.1.RHEL7
cd couchbase.5.0.1.RHEL7
repo init -u git://github.com/couchbase/manifest -m couchbase-server/spock/5.0.1.xml
repo sync
mkdir build
cd build
vim /opt/couchbase/bin/couchbase-server
#ENTERPRISE=false
export http_proxy=proxy.teligent.ru:1111
cmake  -G "Unix Makefiles" -D PRODUCT_VERSION:STRING="4.5.1-2844" -D BUILD_ENTERPRISE:BOOL=false -D CMAKE_BUILD_TYPE=RelWithDebInfo -D CMAKE_INSTALL_PREFIX=/opt/couchbase ..
#скачает зависимости
-- Configuring done
-- Generating done
-- Build files have been written to: /Users/paf/Documents/couchbase.5.0.1.RHEL7/build

make -j6 install
#займёт полчаса примерно
-- Installing: /opt/couchbase/share/man/man1/cbdocloader.1
#

cd /
mkdir -p ~/rpmbuild/SOURCES
tar -czvf ~/rpmbuild/SOURCES/couchbase-server-enterprise-to-community-5.0.1-centos7.x86_64.tgz /usr/lib/systemd/system/couchbase-server.service /opt/couchbase

cd ~/rpmbuild/SPECS
rpmbuild -bs couchbase-server-community-5.0.1.spec #если будет упираться, chown root:root на все файлы о которых ругань
rpmbuild -bb couchbase-server-community-5.0.1.spec
Записан: /root/rpmbuild/RPMS/x86_64/couchbase-server-community-5.0.1-5003.x86_64.rpm

#залить
scp /root/rpmbuild/SRPMS/couchbase-server-community-5.0.1*.src.rpm  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
scp /root/rpmbuild/RPMS/x86_64/couchbase-server-community-5.0.1*.x86_64.rpm alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/
#будет доступно http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-community-5.0.1-5003.x86_64.rpm
~~~

merge изменений из couchbase
----------------------------

~~~
cd kv_engine
git remote add couchbase https://github.com/couchbase/kv_engine.git
git pull couchbase <имя branch, который хотите подтянуть; пока не разбирался, как подтягивать в ветке до определённого commit, и вообще делал только для ep, не делал для kv_engine>
#manifest: https://github.com/couchbase/manifest/blob/master/couchbase-server/spock/5.0.1.xml, искать kv_engine, смотреть upstream:
#   <project name="kv_engine" revision="5.0.1" groups="kv"/>
git -am commit
git push
~~~

выкачать исходники
-------------------------------

~~~
cd
mkdir couchbase.5.0.1.teligent.RHEL7
cd couchbase.5.0.1.teligent.RHEL7

function g {
	url="$1"
	folder="$2"
	version="$3"
	git clone "$url/$folder"
	cd "$folder"
	git checkout "$version"
	cd ..
}

сюда клонировать vanilla папку (см. выше)
rm -rf kv_engine
#ключи официальных релизов можно подсматривать в manifest:
#https://github.com/couchbase/manifest/blob/master/couchbase-server/spock/5.0.1.xml

g ssh://git@github.com/teligent-ru kv-engine 5.0.1.teligent.12
~~~

запустить общую сборку, среди прочего получится ep.so, libcJSON
--------------------------------------------------------------------------

~~~
mkdir build
cd build
cmake -G "Unix Makefiles" -D CMAKE_BUILD_TYPE=RelWithDebInfo -D CMAKE_INSTALL_PREFIX=/opt/couchbase -D COUCHBASE_KV_COMMIT_VALIDATION=1 ..
cd ep-engine
make -j6 install
...
Install the project...
-- Install configuration: "RelWithDebInfo"
-- Installing: /opt/couchbase/bin/cbepctl
-- Installing: /opt/couchbase/bin/cbstats
-- Installing: /opt/couchbase/bin/cbcompact
-- Installing: /opt/couchbase/bin/cbvdiff
-- Installing: /opt/couchbase/bin/cbvbucketctl
-- Installing: /opt/couchbase/bin/cbanalyze-core
-- Installing: /opt/couchbase/lib/python/cbepctl
-- Installing: /opt/couchbase/lib/python/cbstats
-- Installing: /opt/couchbase/lib/python/cbcompact
-- Installing: /opt/couchbase/lib/python/cbvdiff
-- Installing: /opt/couchbase/lib/python/cbvbucketctl
-- Installing: /opt/couchbase/lib/python/clitool.py
-- Installing: /opt/couchbase/lib/python/mc_bin_client.py
-- Installing: /opt/couchbase/lib/python/mc_bin_server.py
-- Installing: /opt/couchbase/lib/python/memcacheConstants.py
-- Installing: /opt/couchbase/lib/python/tap.py
-- Installing: /opt/couchbase/lib/python/tap_example.py
-- Installing: /opt/couchbase/share/doc/ep-engine/stats.org
-- Installing: /opt/couchbase/lib/ep.so
-- Set runtime path of "/opt/couchbase/lib/ep.so" to "$ORIGIN/../lib:/opt/couchbase/lib:/opt/couchbase/lib/memcached"
[root@rualpe-vm1 ep-engine]# 
~~~


собрать patch
-------------
~~~
os=7
version=5.0.1
teligent=12
arch=x86_64
a2x --doctype manpage --format manpage kv_engine/engines/ep/cb.asciidoc -D /usr/share/man/man1/
tar -czvf ~/rpmbuild/SOURCES/couchbase-$version-patch-to-$version.teligent.$teligent-centos$os.$arch.tgz /opt/couchbase/lib/{ep.so,libcJSON*} /opt/couchbase/lib/python/cbepctl /usr/share/man/man1/cb.1
#scp ~/rpmbuild/SOURCES/couchbase-$version-patch-to-$version.teligent.$teligent-centos$os.$arch.tgz  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL$os/x86_64/
~~~

выложить результат в виде rpm
-----------------------------

взял spec из сборки community vanilla версии в rpm (см. выше)
~~~
os=7
version=5.0.1
teligent=12
release=5003
cd ~/rpmbuild/SPECS/
cp  couchbase-server-community-$version{,.teligent.$teligent}.spec
vim couchbase-server-community-$version.teligent.$teligent.spec
#поправил на
#Release:       5003.teligent.12
#добавил
#после Source1
Source2: couchbase-5.0.1-patch-to-5.0.1.teligent.12-centos7.x86_64.tgz
#после SOURC1
tar vxzf %{SOURCE2}
#после %files
%attr(0777, root, root) "/usr/share/man/man1/cb.1"
#залил
scp couchbase-server-community-$version.teligent.$teligent.spec alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
#теперь доступно
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/SRPM/couchbase-server-community-5.0.1.teligent.12.spec

rpmbuild -bs couchbase-server-community-$version.teligent.$teligent.spec #если будет упираться, chown root:root на все файлы о которых ругань
rpmbuild -bb couchbase-server-community-$version.teligent.$teligent.spec

#залить
scp /root/rpmbuild/SRPMS/couchbase-server-community-$version-$release.teligent.$teligent.src.rpm  alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
scp /root/rpmbuild/RPMS/x86_64/couchbase-server-community-$version-$release.teligent.$teligent.x86_64.rpm alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL$os/x86_64/
~~~

ссылка для скачивания rpm
-------------------------
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-community-5.0.1-5003.teligent.12.x86_64.rpm

ссылка для скачивания srpm (там spec ещё раз)
---------------------------------------------
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/SRPM/couchbase-server-community-5.0.1-5003.teligent.12.src.rpmhttp://gigant.teligent.ru/kickstarts/3RD_PARTY/

сводная инструкция на конечном узле
-----------------------------------
man cb
