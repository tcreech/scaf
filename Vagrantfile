# -*- mode: ruby -*-
# vi: set ft=ruby :

# Vagrantfile API/syntax version. Don't touch unless you know what you're doing!
VAGRANTFILE_API_VERSION = "2"

Vagrant.configure(VAGRANTFILE_API_VERSION) do |config|

  # Distribution choices:
  #config.vm.box = "ubuntu/trusty64"
  config.vm.box = "debian/jessie64"

  config.vm.provision "shell", inline: "apt-get install -y git build-essential libhwloc-dev libzmq3-dev libpapi-dev automake autoconf libtool"
  # Take advantage of distributed SCM: make our own local copy
  config.vm.provision "shell", inline: "git clone /vagrant scaf"
  config.vm.provision "shell", inline: "cd scaf && autoreconf -fi"
  config.vm.provision "shell", inline: "cd scaf && ./configure"
  config.vm.provision "shell", inline: "make -C scaf install"

end
