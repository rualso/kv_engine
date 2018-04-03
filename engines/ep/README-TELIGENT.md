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

общие переменные bash
--------------
~~~
os=7
version=5.0.1
release=5003
teligent=15
arch=x86_64
~~~

восстановление rpm спеки
---------------------------------

https://packages.couchbase.com/releases/5.0.1/couchbase-server-enterprise-5.0.1-centos7.x86_64.rpm
поставить rpm *без запуска скриптов!* (тогда не создадутся config.dat и прочие, которые потом будут мешать -- падал erlang)
~~~
rpm -ihv --noscripts /Downloads/couchbase-server-enterprise-$version-centos$os.$arch.rpm
~~~
залить
scp /Downloads/couchbase-server-enterprise-$version-centos$os.$arch.rpm alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/
теперь доступно
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-enterprise-5.0.1-centos7.x86_64.rpm

mkdir -p ~/rpmbuild/SPECS
cd ~/rpmbuild/SPECS
#восстановть спеку
rpmrebuild -e couchbase-server
поправить Prefix: строку (не хватает разрыва строки перед вторым Prefix)
после
#SOURCERPM:    couchbase-server-5.0.1-5003.src.rpm
добавить
Source1: couchbase-server-enterprise-5.0.1-centos7.x86_64.tgz

перед %files добавить
%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/
cd $RPM_BUILD_ROOT
#rpm2cpio %{SOURCE0} | cpio -idmv
tar vxzf %{SOURCE1}
:w couchbase-server-enterprise-5.0.1.spec


scp couchbase-server-enterprise-$version.spec alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
#теперь доступно
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/SRPM/couchbase-server-enterprise-5.0.1.spec

mkdir -p ~/rpmbuild/SOURCES
tar -czvf ~/rpmbuild/SOURCES/couchbase-server-enterprise-$version-centos$os.$arch.tgz /opt/couchbase /usr/lib/systemd/system/couchbase-server.service
~~~

merge изменений из couchbase
----------------------------

~~~
cd kv_engine
git remote add couchbase https://github.com/couchbase/kv_engine.git
git pull couchbase <commit, до которого хотите подтянуть>
#manifest: https://github.com/couchbase/manifest/blob/master/couchbase-server/spock/5.0.1.xml, искать kv_engine, смотреть upstream:
<project name="kv_engine" revision="5.0.1" groups="kv"/>
git -am commit
git push
~~~

выкачать исходники
-------------------------------

~~~
cd
mkdir couchbase.$version.teligent.RHEL$os
cd couchbase.$version.teligent.RHEL$os

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

g ssh://git@github.com/teligent-ru kv_engine $version.teligent.$teligent
g ssh://git@github.com/teligent-ru platform $version.teligent.$teligent
g ssh://git@github.com/teligent-ru couchstore $version.teligent.$teligent
cd goproj/src/github.com/couchbase
rm -rf goxdcr
g ssh://git@github.com/teligent-ru goxdcr $version.teligent.$teligent
~~~

проталкивать tag так:
~~~
git tag -d $version.teligent.$teligent
git push origin :refs/tags/$version.teligent.$teligent
git tag $version.teligent.$teligent
git push --tags
~~~

запустить частичную сборку, среди прочего получатся ep.so, libcJSON, libcouchstore.so
-------------------------------------------------------------------------------------

~~~
mkdir build
cd build
cmake  -G "Unix Makefiles" -D PRODUCT_VERSION:STRING="$version-$release" -D BUILD_ENTERPRISE:BOOL=true -D CMAKE_BUILD_TYPE=RelWithDebInfo -D CMAKE_INSTALL_PREFIX=/opt/couchbase ..

cd platform; make install; cd ..
cd kv_engine/engines/ep; make install; cd ../../..
cd couchstore; make install; cd ..
cd goproj/src/github.com/couchbase/goxdcr; make goxdcr/fast; touch all; make install; strip /opt/couchbase/bin/goxdcr; cd ../../../../..
~~~


собрать patch
-------------
~~~
cd ..
a2x --doctype manpage --format manpage kv_engine/engines/ep/cb.asciidoc -D /usr/share/man/man1/
tar -czvf ~/rpmbuild/SOURCES/couchbase-$version-patch-to-$version.teligent.$teligent-centos$os.$arch.tgz /opt/couchbase/bin/goxdcr /opt/couchbase/lib/{ep.so,libcJSON*,libcouchstore.so} /opt/couchbase/lib/python/cbepctl /usr/share/man/man1/cb.1
~~~

выложить результат в виде rpm
-----------------------------

взял spec из сборки community vanilla версии в rpm (см. выше)
~~~
cd ~/rpmbuild/SPECS/
cp  couchbase-server-enterprise-$version.spec couchbase-server-community-$version.teligent.$teligent.spec
vim couchbase-server-community-$version.teligent.$teligent.spec
#переделать из enterprise в community внешне, чтобы было можно rpm -U сделать
 BuildArch:     x86_64
-Name:          couchbase-server
+Name:          couchbase-server-community
-Conflicts:     couchbase-server-community  
-Provides:      couchbase-server = 5.0.1-5003
-Provides:      couchbase-server(x86-64) = 5.0.1-5003
+Conflicts:     couchbase-server  
+Provides:      couchbase-server-community = 5.0.1-5003
+Provides:      couchbase-server-community(x86-64) = 5.0.1-5003
#поправил на
#Release:       5003.teligent.15
#добавил
#после Source1
Source2: couchbase-5.0.1-patch-to-5.0.1.teligent.15-centos7.x86_64.tgz
#после SOURCE1
tar vxzf %{SOURCE2}
#после %files
%attr(0777, root, root) "/usr/share/man/man1/cb.1"
#залил
scp couchbase-server-community-$version.teligent.$teligent.spec alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/SRPM/
#теперь доступно
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/SRPM/couchbase-server-community-5.0.1.teligent.15.spec

rpmbuild -bb couchbase-server-community-$version.teligent.$teligent.spec
scp /root/rpmbuild/RPMS/x86_64/couchbase-server-community-$version-$release.teligent.$teligent.$arch.rpm alexander.petrossian@gigant:/var/www/kickstarts/3RD_PARTY/couchbase/RHEL$os/$arch/
~~~

проверка полученной rpm
-----------------------
~~~
rpm -e couchbase-server
#rpm -e couchbase-server-enterprise #иногда так надо
rm -rf /opt/couchbase
rpm -ihv ~/rpmbuild/RPMS/$arch/couchbase-server-community-$version-$release.teligent.$teligent.$arch.rpm
~~~

ссылка для скачивания rpm
-------------------------
http://gigant.teligent.ru/kickstarts/3RD_PARTY/couchbase/RHEL7/x86_64/couchbase-server-community-5.0.1-5003.teligent.15.x86_64.rpm

сводная инструкция на конечном узле
-----------------------------------
man cb
