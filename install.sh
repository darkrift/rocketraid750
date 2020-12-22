#!/bin/bash

if test $(id -u) != "0"; then
  echo "Please supply root's password to install this package. "
  su -c "$0"
  ret=$?
  exit $ret
fi

pidfile=/var/run/yum.pid

export DEBIAN_FRONTEND=noninteractive

checkyum()
{
  if test -f "$pidfile"; then
    if test "$1" = "check"; then
      echo "YUM is being used: enter 'q' to exit the kernel-devel list."
    else
      echo "YUM is being used: enter 'q' to exit the $1 installation."
    fi
    while test -f "$pidfile"; do
      echo -n .
      read -t 1 c
      if test "${c}" = "q"; then
        exit 1
      fi
    done
    echo
  fi
}

installpkgcentos()
{
  if ! type $1 >/dev/null 2>&1; then
    if ! rpm -qe --quiet $1; then
      echo "Installing required package: $1"
      checkyum $1
      yum -y --quiet install ${1} >/dev/null 2>&1
      if test $? != 0; then
        echo "Failed to install package $1."
        return 1
      fi
    fi
  fi
  return 0
}

checkapt()
{
  lockfile=/var/lib/dpkg/lock

  if test -f "$lockfile"; then
    if lsof "$lockfile" >/dev/null 2>&1 ;then
      if test "$1" = "check"; then
        echo "APT is being used: enter 'q' to exit the linux-headers list."
      else
        echo "APT is being used: enter 'q' to exit the $1 installation."
      fi
    fi
    while lsof "$lockfile" >/dev/null 2>&1; do
      echo -n .
      read -t 1 c
      if test "${c}" = "q"; then
        return 1
      fi
    done
  fi
  return 0
}

installpkgdebian()
{
  if ! type $1 >/dev/null 2>&1; then
    echo "Installing required package: $1"
    checkapt $1
    apt-get -y -qq install ${1} #>/dev/null 2>&1
    if test $? != 0; then
      echo "Failed to install package $1."
      return 1
    fi
  fi
  return 0
}

checkpkg()
{
  if ! type $1 > /dev/null 2>&1; then
    return 1
  else
    return 0
  fi
}

downloadinstall()
{
  name=$(basename "$1")
  baseurl=$(dirname "$1")
  rm -f /var/tmp/${name}
  if type curl > /dev/null 2>&1; then
    if curl --connect-timeout 10 -s "${baseurl}/" 2>&1 | grep -s -q "${name}"; then
      echo "Downloading package ${name} from ${baseurl}."
      curl --connect-timeout 10 -s -o /var/tmp/${name} "${baseurl}/${name}"
      if test $? = 0; then
        echo "Installing package ${name} from downloader."
        checkyum kernel-devel
        yum -y --quiet localinstall /var/tmp/${name} >/dev/null 2>&1
        rm -f /var/tmp/${name}
       return 0
      fi
    fi
  fi
  return 1
}

installkerneldcentos()
{
  cdroms=$(ls /dev/sr* 2>/dev/null)
  if test "${cdroms}" != ""; then
    for i in ${cdroms}; do
      mkdir -p /var/tmp/cdrom
      if test -e $i; then
        mount $i /var/tmp/cdrom > /dev/null 2>&1
        if test $? != "0"; then
          rmdir /var/tmp/cdrom
          continue
        fi

        if test -f /var/tmp/cdrom/.discinfo; then
          break
       else
          umount /var/tmp/cdrom
          rmdir /var/tmp/cdrom
        fi
      fi
    done
  fi

  #if ! test -d /var/tmp/cdrom; then
  #  echo "Installation media was not found."
  #fi

  kernels=$(rpm -qa kernel)

  if test "$kernels" = ""; then
    return 0
  fi
  kernelds=$(rpm -qa kernel-devel)
  checkyum check
  reps=$(yum --quiet --showduplicates list kernel-devel 2>/dev/null | grep ^Avail -A 100 | grep kernel-devel)

  for kernel in ${kernels}; do
    kerneld=${kernel/kernel/kernel-devel}
    if echo "${kernelds}" | grep -s -q ^${kerneld}$; then
      continue
    fi

    pkgversion=${kernel/kernel-}
    pkgversion=$(echo ${pkgversion} | rev | cut -d. -f2- | rev)
    inreps=0
    if test "$pkgversion" != ""; then
      if echo "${reps}" | grep -s -q "${pkgversion}"; then
        inreps=1
      fi
    fi

    if test -f /var/tmp/cdrom/Packages/${kerneld}.rpm; then
      checkyum kernel-devel
      yum -y --quiet localinstall /var/tmp/cdrom/Packages/${kerneld}.rpm >/dev/null 2>&1
      if test $? != 0; then
        echo "Failed to install package ${kerneld}.rpm."
      fi
    elif test "$inreps" = "1"; then
      echo "Installing package ${kerneld}.rpm from yum repository."
      checkyum kernel-devel
      yum -y --quiet install ${kerneld} >/dev/null 2>&1
      if test $? != 0; then
        echo "Failed to install package ${kerneld}.rpm."
      fi
    elif test -e /etc/centos-release; then
      if echo ${pkgversion} | grep -s -q 2\\.6\\.32; then
        maj=6
        rev=$(echo ${pkgversion} | cut -d- -f2 | cut -d. -f1)
        dir=Packages
        if echo ${kerneld} | grep -s -q x86_64; then
          arch=x86_64
        else
          arch=i386
        fi
        case "${rev}" in
          71)
            dir=RPMS
            min=0
            ;;
          131)
            min=1
            ;;
          220)
            min=2
            ;;
          279)
            min=3
            ;;
          358)
            min=4
            ;;
          431)
            min=5
            ;;
          *)
            min=""
            ;;
        esac

        if test "${min}" = ""; then
          continue
        fi

        if test "${min}" -gt 3; then
          downloadinstall http://mirrors.kernel.org/centos/${maj}.${min}/os/${arch}/${dir}/${kerneld}.rpm
          if test $? = 0; then
            continue
          fi

          downloadinstall http://mirrors.kernel.org/centos/${maj}.${min}/updates/${arch}/${dir}/${kerneld}.rpm
          if test $? = 0; then
            continue
          fi
        fi

        downloadinstall http://mirror.symnds.com/distributions/CentOS-vault/${maj}.${min}/os/${arch}/${dir}/${kerneld}.rpm
        if test $? = 0; then
          continue
        fi

        downloadinstall http://mirror.symnds.com/distributions/CentOS-vault/${maj}.${min}/updates/${arch}/${dir}/${kerneld}.rpm
        if test $? = 0; then
          continue
        fi

        downloadinstall http://vault.centos.org/${maj}.${min}/os/${arch}/${dir}/${kerneld}.rpm
        if test $? = 0; then
          continue
        fi

        downloadinstall http://vault.centos.org/${maj}.${min}/updates/${arch}/${dir}/${kerneld}.rpm
        if test $? = 0; then
          continue
        else
          echo "Failed to download or install package ${kerneld}.rpm."
        fi
      fi
      continue
    fi
  done

  if test -d /var/tmp/cdrom; then
    umount /var/tmp/cdrom
    rmdir /var/tmp/cdrom
  fi
}

installkernelddebian()
{
  for i in /boot/vmlinuz-*; do
    #dpkg -S $i 2>/dev/null | cut -d: -f1
    pkg=`dpkg -S $i 2>/dev/null | cut -d: -f1`
    if test "$pkg" != ""; then
      #pkg=`echo $pkg`
      #pkg=`echo $pkg`
      pkgd=${pkg/image/headers}
      checkapt $pkgd
      echo "Installing package ${pkgd} from APT repository."
      apt-get -y -qq install $pkgd
    fi
  done
}

#MODNAME=`grep TARGETNAME product/*/linux/Makefile | sed s'# ##'g | cut -d\= -f2 | sed s'#\r##'`
echo "Starting install R750 Linux Open Source package ............"
MODNAME=r750

if test "$NOCOMPINSTALL" = ""; then
  if test -e /etc/redhat-release; then
    installpkgcentos gcc
    checkpkg gcc
    if test $? != 0; then
      echo "Package gcc not installed and failed to install. exiting ..."
      exit 1
    fi

    installpkgcentos make
    checkpkg make
    if test $? != 0; then
      echo "Package make not installed and failed to install. exiting ..."
      exit 1
    fi

    installpkgcentos perl
    checkpkg perl
    if test $? != 0; then
      echo "Package perl not installed and failed to install. exiting ..."
      exit 1
    fi

    installkerneldcentos
  elif test -e /etc/debian_version; then
    installpkgdebian gcc
    checkpkg gcc
    if test $? != 0; then
      echo "Package gcc not installed and failed to install. exiting ..."
      exit 1
    fi

    installpkgdebian make
    checkpkg make
    if test $? != 0; then
      echo "Package make not installed and failed to install. exiting ..."
      exit 1
    fi

    installpkgdebian perl
    checkpkg perl
    if test $? != 0; then
      echo "Package perl not installed and failed to install. exiting ..."
      exit 1
    fi

    installkernelddebian
  else
    echo "Current running system is not a CentOS 6/Debian system."
    echo -n "Do you want to continue the installation? (y/n): "
    read ans
    if test "$ans" != "y"; then
      exit 1
    fi

    checkpkg gcc
    if test $? != 0; then
      echo "Package gcc not installed. exiting ..."
      exit 1
    fi

    checkpkg make
    if test $? != 0; then
      echo "Package make not installed. exiting ..."
      exit 1
    fi

    checkpkg perl
    if test $? != 0; then
      echo "Package perl not installed. exiting ..."
      exit 1
    fi
  fi
fi

if [ "$MODNAME" == "" ]; then
  echo "Could not determine driver name"
fi

rm -rf product/*/linux/*.ko
rm -rf product/*/linux/.build

rm -rf /usr/share/hptdrv/$MODNAME
mkdir -p /usr/share/hptdrv/
cp -R `pwd` /usr/share/hptdrv/$MODNAME
rm -f /usr/share/hptdrv/$MODNAME/install.sh
rm -rf /usr/share/hptdrv/$MODNAME/dist
chown -R 0:0 /usr/share/hptdrv/$MODNAME

#touch /etc/sysconfig/hptdrv || exit 1
#echo MODLIST=\"$MODNAME\" > /etc/sysconfig/hptdrv

if test -d /etc/rc.d/init.d; then
  rm -f /etc/rc.d/init.d/hptdrv-monitor
  install -m 755 -o root -g root dist/hptdrv-monitor /etc/rc.d/init.d/
  
  if test -e /etc/debian_version; then  
    rm -f /etc/rc.d/init.d/hptmod
    install -m 755 -o root -g root dist/hptmod /etc/rc.d/init.d/
    str_sed=`sed -n '/Required-Start:.*hptmod/p' /etc/rc.d/init.d/udev`
    if test "$str_sed" = "" ;then
  	  sed -i '/Required-Start:/s/.*/&hptmod/' /etc/rc.d/init.d/udev
    fi
  fi
elif test -d /etc/init.d; then
  rm -f /etc/init.d/hptdrv-monitor
  install -m 755 -o root -g root dist/hptdrv-monitor /etc/init.d/

  if test -e /etc/debian_version; then
    rm -f /etc/init.d/hptmod
    install -m 755 -o root -g root dist/hptmod /etc/init.d/
    str_sed=`sed -n '/Required-Start:.*hptmod/p' /etc/init.d/udev`
    if test "$str_sed" = "" ;then
  	  sed -i '/Required-Start:/s/.*/&hptmod/' /etc/init.d/udev
    fi
  fi
fi
install -m 755 -o root -g root dist/hptdrv-rebuild /usr/sbin/

if test -d /var/lock/subsys; then
  touch /var/lock/subsys/hptdrv-monitor
fi

if type update-rc.d >/dev/null 2>&1; then
  update-rc.d -f hptdrv-monitor remove >/dev/null 2>&1
  update-rc.d hptdrv-monitor defaults >/dev/null 2>&1

  if test -e /etc/debian_version; then 
    update-rc.d -f hptmod remove >/dev/null 2>&1
    if test -s /etc/init.d/.depend.boot; then
      update-rc.d hptmod defaults >/dev/null 2>&1
      update-rc.d hptmod enable S >/dev/null 2>&1
    else
      # start it before udev
      update-rc.d hptmod start 03 S . >/dev/null 2>&1
    fi
  fi
else
  if test -d /etc/rc.d/rc0.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc0.d/K10hptdrv-monitor
  fi
  if test -d /etc/rc.d/rc6.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc6.d/K10hptdrv-monitor
  fi
  if test -d /etc/rc.d/rc1.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc1.d/S10hptdrv-monitor
  fi
  if test -d /etc/rc.d/rc2.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc2.d/S10hptdrv-monitor
  fi
  if test -d /etc/rc.d/rc3.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc3.d/S10hptdrv-monitor
  fi
  if test -d /etc/rc.d/rc4.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc4.d/S10hptdrv-monitor
  fi
  if test -d /etc/rc.d/rc5.d; then
    ln -sf ../init.d/hptdrv-monitor /etc/rc.d/rc5.d/S10hptdrv-monitor
  fi
fi

OS=
[ -f /etc/lsb-release ] && OS=`sed -n '/DISTRIB_ID/p' /etc/lsb-release | cut -d'=' -f2 | tr [[:upper:]] [[:lower:]]` 
if [ "${OS}" = "ubuntu" ] ;then
  type upstart-udev-bridge > /dev/null  2>&1
  if [ "$?" = 0 ] ;then
    install -m 755 -o root -g root dist/hptdrv.conf /etc/init/
    install -m 755 -o root -g root dist/udev.override /etc/init/
  fi 
fi

if type systemctl >/dev/null 2>&1; then
  rm -f /lib/systemd/system/systemd-hptdrv.service
  rm -f /usr/lib/systemd/system/systemd-hptdrv.service

  if test -d /lib/systemd/system; then
    install -m 644 -o root -g root dist/systemd-hptdrv.service /lib/systemd/system/
    install -m 644 -o root -g root dist/hptdrv-monitor.service /lib/systemd/system/
  elif test -d /usr/lib/systemd/system; then
    # suse 13.1 bug
    if test -f "/usr/lib/systemd/system/network@.service"; then
      mkdir -p "/usr/lib/systemd/system/network@.service.d/"
      install -m 644 -o root -g root dist/50-before-network-online.conf "/usr/lib/systemd/system/network@.service.d/"
    fi
    install -m 644 -o root -g root dist/hptdrv-monitor.service /usr/lib/systemd/system/
    install -m 644 -o root -g root dist/systemd-hptdrv.service /usr/lib/systemd/system/
  fi

  systemctl daemon-reload >/dev/null 2>&1
  systemctl enable hptdrv-monitor.service >/dev/null 2>&1
  systemctl enable systemd-hptdrv >/dev/null 2>&1
  systemctl start hptdrv-monitor.service >/dev/null 2>&1
  rm -f /etc/init.d/hptmod
  install -m 755 -o root -g root dist/hptmod /etc/init.d/
fi 

if [ -d /etc/initramfs-tools/scripts/init-top ]; then
  install -m 755 -o root -g root dist/hptdrv /etc/initramfs-tools/scripts/init-top/
fi

#if [ -d /etc/initramfs-tools/conf.d ];then
#  file_p=/etc/initramfs-tools/conf.d/driver-policy
#  conf=""
#  if [ -f ${file_p} ] ;then
#    conf=`sed -n '/^MODULES=[a-z]*/p' ${file_p}`
#  fi
#  if [ "${conf}" = "" ] ;then
#    echo MODULES=dep >> ${file_p} 
#  else
#    sed -i 's/^MODULES=[a-z]*/MODULES=dep/' ${file_p}
#  fi
#fi

#if [ -d /etc/dracut.conf.d ]; then
#  echo "omit_drivers+=$MODNAME" > /etc/dracut.conf.d/${MODNAME}.conf
#fi

echo "Checking and building device driver for R750............"
env MODINSTALL=1 /etc/init.d/hptdrv-monitor start

if test "$NOCOMPINSTALL" = ""; then
  env modules="${MODNAME}" /etc/init.d/hptdrv-monitor force-stop
  if test $? != 0; then
    echo "Driver r750 for some kernel built failed, please refer to file /var/log/hptdrv.log"
  fi
fi

# vim: expandtab ts=2 sw=2 ai
