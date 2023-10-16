#!/bin/sh

stop_bg() {
  sleep 2
  ps aux | grep "$EXEC" | awk '{print $2}' | xargs kill -TERM > /dev/null 2>&1
}

ARGS="$@"

EXEC="flutter-auto"
which flutter-auto && EXEC=$(which flutter-auto)

echo "Test decimal:"

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

echo "Test hex:"

for i in `seq 0 255`; do
  stop_bg &
  hex="0x$(printf "%X" $i)"
  ret=$($EXEC $ARGS --a=$hex 2>&1 | grep "Accessibility Features:")

  if [ "$i" = "$(echo "$ret" | sed "s|.*Accessibility Features: ... \(.*\)|\1|g")" ]; then
    echo "OK: $hex"
  elif [ "$i" -gt 127 ]; then
    if [ "$(expr $i - 128)" = "$(echo "$ret" | sed "s|.*Accessibility Features: ... \(.*\)|\1|g")" ]; then
      echo "OK: $hex"
    else
      echo "NG: $hex"
    fi
  else
    echo "NG: $hex"
  fi
done

echo "Test octet:"

for i in `seq 0 255`; do
  stop_bg &
  oct="0$(printf "%o" $i)"
  ret=$($EXEC $ARGS --a=$oct 2>&1 | grep "Accessibility Features:")

  if [ "$i" = "$(echo "$ret" | sed "s|.*Accessibility Features: ... \(.*\)|\1|g")" ]; then
    echo "OK: $oct"
  elif [ "$i" -gt 127 ]; then
    if [ "$(expr $i - 128)" = "$(echo "$ret" | sed "s|.*Accessibility Features: ... \(.*\)|\1|g")" ]; then
      echo "OK: $oct"
    else
      echo "NG: $oct"
    fi
  else
    echo "NG: $oct"
  fi
done
