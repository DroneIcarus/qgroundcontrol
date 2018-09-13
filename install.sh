#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status.
set -e
# Print working directory
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

check_root()
{
    SCRIPT_USER="$(whoami)"
    if [ "$SCRIPT_USER" == "root" ]
    then
        echo "You need to run the script as root"
        echo "Example: sudo .$DIR/install.sh"
        return 1
    fi
}

check_os_disto()
{
	echo '# Checking OS and distribution'
	echo '######################################################'
	OS="$(uname -s)"
	case "${OS}" in
		"Linux")
			DISTRO="$( awk '/^ID=/' /etc/*-release | awk -F'=' '{ print tolower($2) }' | sed -e 's/^"//' -e 's/"$//' )"
			DISTRO_VER="$( awk '/^DISTRIB_RELEASE=/' /etc/*-release | awk -F'=' '{ print tolower($2) }')"
			if ! [[ "$DISTRO" =~ ^(ubuntu)$ ]]
			then
				echo "Error: Your linux distribution is not supported."
				exit 1
			fi
			;;
		"Darwin")
			DISTRO="mac"
			#TODO: Fetch macOS version for DISTRO_VER
			echo "Error: macOS is not supported."
			;;
		*)
			echo "Error: Your OS is not supported."
			exit 1
			;;
	esac
	echo "$OS" "$DISTRO" "$DISTRO_VER"
}

install_distro_packages()
{
	echo '# Installing distribution prerequesites for QGroundControl'
	echo '##########################################################'
	case "$1" in
	"ubuntu")
		sudo apt-get update
		#Qt
		sudo apt-get install build-essential libfontconfig1 mesa-common-dev libglu1-mesa-dev \
					speech-dispatcher geoclue libudev-dev libsdl2-dev -y #QGC
		;;
	*)
		echo "Error: Your linux distribution is not supported."
		exit 1
		;;
	esac
}

configure_environment()
{
	echo '# Configuring QGroundControl environment...'
	echo '##############################################'
	git submodule init && git submodule update # Must be in QGroundControl git directory
	touch $DIR/user_config.pri
	
	sudo usermod -a -G dialout $USER
	sudo apt-get remove modemmanager -y
}

install_qt()
{
	echo '# Downloading and installing Qt-Creator'
	echo '#######################################'
	echo ''
	echo '*********************************************************************************'
	echo '** Please make sure you install the latest kit and QT chart from the installer **'
	echo '**          If not, you can always install it from QT MaintenanceTool          **'
	echo '*********************************************************************************'
	echo ''
	wget http://download.qt.io/official_releases/online_installers/qt-unified-linux-x64-online.run
	chmod +x qt-unified-linux-x64-online.run
	./qt-unified-linux-x64-online.run
}


###
# Main installer script
###
echo '
###
#    icarUS Mission Optimizer installer
###
'
#check_root
check_os_disto
install_distro_packages "$DISTRO"

configure_environment
install_qt

echo '# You should now be all set!'
echo ''
echo 'You can now open up QT-Creator, select the .pro project and build/run it'
