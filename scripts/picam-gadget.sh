#!/bin/bash

# Variables we need to make things easier later on.

CONFIGFS="/sys/kernel/config"
GADGET="$CONFIGFS/usb_gadget"
VID="0x0525"
PID="0xa4a2"
SERIAL="0123456789"
MANUF=$(hostname)
PRODUCT="PiCam (UVC+NCM)"
BOARD=$(strings /proc/device-tree/model)
UDC=`ls /sys/class/udc` # will identify the 'first' UDC

# Later on, this function is used to tell the usb subsystem that we want
# to support a particular format, framesize and frameintervals
create_frame() {
	# Example usage:
	# create_frame <function name> <width> <height> <format> <name> <intervals>

	FUNCTION=$1
	WIDTH=$2
	HEIGHT=$3
	FORMAT=$4
	NAME=$5

	wdir=functions/$FUNCTION/streaming/$FORMAT/$NAME/${HEIGHT}p

	mkdir -p $wdir
	echo $WIDTH > $wdir/wWidth
	echo $HEIGHT > $wdir/wHeight
	echo $(( $WIDTH * $HEIGHT * 2 )) > $wdir/dwMaxVideoFrameBufferSize
	cat <<EOF > $wdir/dwFrameInterval
$6
EOF
}

# This function sets up the UVC gadget function in configfs and binds us
# to the UVC gadget driver.
create_uvc() {
	CONFIG=$1
	FUNCTION=$2

	echo "	Creating UVC gadget functionality : $FUNCTION"
	mkdir functions/$FUNCTION

	create_frame $FUNCTION 640 480 uncompressed u "333333
416667
500000
666666
1000000
1333333
2000000
"
	create_frame $FUNCTION 1280 720 uncompressed u "1000000
1333333
2000000
"
	create_frame $FUNCTION 1920 1080 uncompressed u "2000000"
#	create_frame $FUNCTION 640 480 mjpeg m "333333
#416667
#500000
#666666
#1000000
#1333333
#2000000
#"
#	create_frame $FUNCTION 1280 720 mjpeg m "333333
#416667
#500000
#666666
#1000000
#1333333
#2000000
#"
#	create_frame $FUNCTION 1920 1080 mjpeg m "333333
#416667
#500000
#666666
#1000000
#1333333
#2000000
#"

	mkdir functions/$FUNCTION/streaming/header/h
	cd functions/$FUNCTION/streaming/header/h
	ln -s ../../uncompressed/u
	ln -s ../../mjpeg/m
	cd ../../class/fs
	ln -s ../../header/h
	cd ../../class/hs
	ln -s ../../header/h
	cd ../../class/ss
	ln -s ../../header/h
	cd ../../../control
	mkdir header/h
	ln -s header/h class/fs
	ln -s header/h class/ss
	cd ../../../

	# This configures the USB endpoint to allow 3x 1024 byte packets per
	# microframe, which gives us the maximum speed for USB 2.0. Other
	# valid values are 1024 and 2048, but these will result in a lower
	# supportable framerate.
	echo 2048 > functions/$FUNCTION/streaming_maxpacket

	ln -s functions/$FUNCTION configs/c.1
}

create_ethernet_ncm() {
    CONFIG=$1
    FUNCTION=$2 # Bu "ncm.0" olacak

    echo "	Creating CDC-NCM Ethernet functionality: $FUNCTION"
    mkdir functions/$FUNCTION

     echo "48:6f:70:10:ff:5b" > functions/$FUNCTION/host_addr
    echo "48:6f:70:10:ff:5c" > functions/$FUNCTION/dev_addr

    # Ethernet fonksiyonunu da kamerayla aynı yapılandırmaya bağla.
    ln -s functions/$FUNCTION $CONFIG
}

delete_uvc() {
    CONFIG=$1
    FUNCTION=$2

    echo "	Deleting UVC gadget function: $FUNCTION"
    rm -f $CONFIG/$FUNCTION

    rm functions/$FUNCTION/control/class/*/h
	rm functions/$FUNCTION/streaming/class/*/h
	rm functions/$FUNCTION/streaming/header/h/u
	rmdir functions/$FUNCTION/streaming/uncompressed/u/*/
	rmdir functions/$FUNCTION/streaming/uncompressed/u
	rm -rf functions/$FUNCTION/streaming/mjpeg/m/*/
	rm -rf functions/$FUNCTION/streaming/mjpeg/m
	rmdir functions/$FUNCTION/streaming/header/h
	rmdir functions/$FUNCTION/control/header/h
	rmdir functions/$FUNCTION

}

delete_ethernet_ncm() {
    CONFIG=$1
    FUNCTION=$2

    echo "	Deleting CDC-NCM Ethernet function: $FUNCTION"
    rm -f $CONFIG/$FUNCTION

    rmdir functions/$FUNCTION
}

# ================================================================

# This loads the module responsible for allowing USB Gadgets to be
# configured through configfs, without which we can't connect to the
# UVC gadget kernel driver
echo "Loading composite module"
modprobe libcomposite

# This section configures the gadget through configfs. We need to
# create a bunch of files and directories that describe the USB
# device we want to pretend to be.

case "$1" in
    start)
    if [ ! -d $GADGET/g1 ]; then
    	echo "Detecting platform:"
    	echo "	board : $BOARD"
    	echo "	udc 	: $UDC"

    	echo "Creating the USB gadget"

    	echo "Creating gadget directory g1"
    	mkdir -p $GADGET/g1

    	cd $GADGET/g1
    	if [ $? -ne 0 ]; then
    		echo "Error creating usb gadget in configfs"
    		exit 1;
    	else
    		echo "OK"
    	fi

    	echo "Setting Vendor and Product ID's"
    	echo $VID > idVendor
    	echo $PID > idProduct
    	echo "OK"

    	echo "Setting English strings"
    	mkdir -p strings/0x409
    	echo $SERIAL > strings/0x409/serialnumber
    	echo $MANUF > strings/0x409/manufacturer
    	echo $PRODUCT > strings/0x409/product
    	echo "OK"

    	echo "Creating Config"
    	mkdir -p configs/c.1/strings/0x409
        echo "UVC Camera + NCM Ethernet" > configs/c.1/strings/0x409/configuration
    	echo "Creating functions..."

    	create_uvc configs/c.1 uvc.0
    
        create_ethernet_ncm configs/c.1 ncm.0	
    
        echo "OK"

    
    	echo "Binding USB Device Controller"
    	echo $UDC > UDC
    	echo "OK"
    fi
    ;;
    stop)
    echo "Stopping the USB gadget"
    set +e # Ignore all errors here on a best effort
    cd $GADGET/g1
    if [ $? -ne 0 ]; then
        echo "Error: no configfs gadget found"
        exit 1;
    fi
    echo "Unbinding USB Device Controller"
    grep $UDC UDC && echo "" > UDC
    echo "OK"
    delete_uvc configs/c.1 uvc.0
    delete_ethernet_ncm configs/c.1 ncm.0
    echo "Removing gadget directory g1"
    cd ..
    rmdir g1
    echo "OK"
    ;;
    *)
    echo "Usage: $0 {start|stop}"
esac

