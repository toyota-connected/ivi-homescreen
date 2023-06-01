#!/bin/sh

stop_bg() {
  sleep 2
  ps aux | grep "$EXEC" | awk '{print $2}' | xargs kill -TERM > /dev/null 2>&1
}

ARGS="--j=$1"

EXEC="homescreen"
which homescreen && EXEC=$(which homescreen)
which flutter-auto && EXEC=$(which flutter-auto)

for i in `seq 0 255`; do
  stop_bg &
  ret=$($EXEC $ARGS --a=$i 2>&1 | grep "Accessibility Features:")

  if [ "$i" = "$(echo "$ret" | sed "s|.*Accessibility Features: ... \(.*\)|\1|g")" ]; then
    echo "OK: $i"
  elif [ "$i" -gt 127 ]; then
    if [ "$(expr $i - 128)" = "$(echo "$ret" | sed "s|.*Accessibility Features: ... \(.*\)|\1|g")" ]; then
      echo "OK: $i"
    else
      echo "NG: $i"
    fi
  else
    echo "NG: $i"
  fi
done
