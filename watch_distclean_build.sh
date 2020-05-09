#!/bin/bash

AGL_SSH="root@192.168.1.20"
AGL_SSH_PORT="2222"
NEEDS_BUILD_FILE=./needsbuild

for (( ; ; ))
do
    if [ -f "$NEEDS_BUILD_FILE" ]; then
        rm "$NEEDS_BUILD_FILE"

        # Clean build
        ./autobuild/agl/autobuild distclean
        ./autobuild/agl/autobuild package

        # Parse app name & version from config
        APP_NAME=$(grep '<widget ' build/package/config.xml | perl -pe 's|<widget .*?id="(.*?)".*|\1|')
        APP_VERSION=$(grep '<widget ' build/package/config.xml | perl -pe 's|<widget .*?version="(.*?)".*|\1|')

        # Delete old wgt; copy new wgt place
	    ssh $AGL_SSH "-p $AGL_SSH_PORT" "rm ~/$APP_NAME.wgt"
        sleep 0.1
        scp "-P $AGL_SSH_PORT" "build/$APP_NAME.wgt" "$AGL_SSH:~/"
        sleep 0.1

        # Kill and uninstall old process
        ssh $AGL_SSH "-p $AGL_SSH_PORT" "su -c \"afm-util kill $APP_NAME@$APP_VERSION\" agl-driver"
        sleep 0.1
	    ssh $AGL_SSH "-p $AGL_SSH_PORT" "su -c \"afm-util remove $APP_NAME@$APP_VERSION\" agl-driver"
        sleep 0.1

        # Install and start new process
	    ssh $AGL_SSH "-p $AGL_SSH_PORT" "afm-util install $APP_NAME.wgt"
        sleep 0.1
        ssh $AGL_SSH "-p $AGL_SSH_PORT" "su -c \"afm-util start $APP_NAME@$APP_VERSION\" agl-driver"
    fi
    sleep 0.5
done