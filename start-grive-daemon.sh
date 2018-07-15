#!/bin/bash
(sleep 5; grive-daemon) &
(while [ true ]; do sleep 3600; date --utc +%FT%TZ > ~/Google\ Drive/grive_autosync_time; done) &
