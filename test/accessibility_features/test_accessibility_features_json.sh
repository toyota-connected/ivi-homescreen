#!/bin/sh

stop_bg() {
  sleep 2
  ps aux | grep "$EXEC" | awk '{print $2}' | xargs kill -TERM > /dev/null 2>&1
}

ABS_PATH=$(readlink -f $1)
BASENAME=$(basename ${ABS_PATH})

ARGS="--j=/tmp/${BASENAME}"

EXEC="flutter-auto"
which flutter-auto && EXEC=$(which flutter-auto)
which flutter-auto && EXEC=$(which flutter-auto)

for i in `seq 0 255`; do
  stop_bg &
  cp ${ABS_PATH} /tmp/${BASENAME}
  sed -i "s|%%ACCESSIBILITY%%|${i}|g" /tmp/${BASENAME}
  ret=$($EXEC $ARGS 2>&1 | grep "Accessibility Features:")

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
