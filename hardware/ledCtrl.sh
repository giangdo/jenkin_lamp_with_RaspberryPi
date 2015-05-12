#!/bin/bash

# SANNITY CHECK FOR INPUT 
if [ "$#" -ne 3 ]; then
   echo "Please specify: ledNumber - state"
   echo "example:"
   echo "./ledCtrl.sh cphw red on"
   echo "./ledCtrl.sh plex blu on"
   echo "./ledCtrl.sh pes yel off"
   echo "./ledCtrl.sh cphw all on"
   echo "./ledCtrl.sh pes all off"
   echo "./ledCtrl.sh gpio 13 on"
   echo "./ledCtrl.sh gpio 26 off"
   exit 0
fi 

# SPECIFY GPIO GROUP
if [ "$1" = "cphw" ]; then
   redLed=13
   bluLed=26
   greLed=19
elif [ "$1" = "pes" ]; then
   redLed=11
   bluLed=6
   greLed=5
elif [ "$1" = "plex" ]; then
   redLed=22
   bluLed=9
   greLed=10
elif [ "$1" = "gpio" ]; then
   redLed=13
   bluLed=26
   greLed=19
else
   echo "please specify gpio group: cphw/pes/plex/gpio"
   exit 0
fi

numberRegex='^[0-9]+$'
# SPECIFY COLOR FROM INPUT
if [ "$2" = "red" ]; then
   led=$redLed
elif [ "$2" = "blu" ]; then
   led=$bluLed
elif [ "$2" = "gre" ]; then
   led=$greLed
elif [ "$2" = "yel" ]; then
   led=$redLed
   led2=$greLed
elif [ "$2" = "all" ]; then
   led=$redLed
   led2=$greLed
   led3=$bluLed
elif [[ "$2" =~ $numberRegex ]]; then
   led=$2
else
   echo "please specify color: red/blu/gre/yel/all/ or gpio number"
   exit 0
fi

# SPECIFY LED STATUS
state=$3

# CONTROL LED 1
gpio=$led
gpioFolder="gpio$gpio"

# Check that gpio folder existed
temp=$(find /sys/class/gpio/ -maxdepth 1 -name $gpioFolder)
if [ "$temp" != "/sys/class/gpio/$gpioFolder" ]; then
   sudo sh -c "echo $gpio > /sys/class/gpio/export"
   temp=$(find /sys/class/gpio/ -maxdepth 1 -name $gpioFolder)
   if [ "$temp" != "/sys/class/gpio/$gpioFolder" ]; then
      echo "can not control this gpio $gpio"
      exit 0 
   fi
fi

# Check current direction
direct=$(cat /sys/class/gpio/$gpioFolder/direction)
if [ "$direct" = "in" ]; then
   sudo sh -c "echo out > /sys/class/gpio/$gpioFolder/direction"
fi

# Turn on or turn off Led depend on input parameter
if [ "$state" = "on" ]
then
   echo "turn on  led $led"
   sudo sh -c "echo 0 > /sys/class/gpio/$gpioFolder/value"
elif [ "$state" = "off" ]
then
   echo "turn off led $led"
   sudo sh -c "echo 1 > /sys/class/gpio/$gpioFolder/value"
else
   echo "input wrong state"
   exit 0
fi

# CONTROL LED 2
if [ -z "$led2" ]; then
   exit 0
else

   gpio=$led2
   gpioFolder="gpio$gpio"

   # Check that gpio folder existed
   temp=$(find /sys/class/gpio/ -maxdepth 1 -name $gpioFolder)
   if [ "$temp" != "/sys/class/gpio/$gpioFolder" ]; then
      sudo sh -c "echo $gpio > /sys/class/gpio/export"
      temp=$(find /sys/class/gpio/ -maxdepth 1 -name $gpioFolder)
      if [ "$temp" != "/sys/class/gpio/$gpioFolder" ]; then
         echo "can not control this gpio $gpio"
         exit 0 
      fi
   fi

   # Check current direction
   direct=$(cat /sys/class/gpio/$gpioFolder/direction)
   if [ "$direct" = "in" ]; then
      sudo sh -c "echo out > /sys/class/gpio/$gpioFolder/direction"
   fi

   # Turn on or turn off Led depend on input parameter
   if [ "$state" = "on" ]
   then
      echo "turn on  led $gpio"
      sudo sh -c "echo 0 > /sys/class/gpio/$gpioFolder/value"
   elif [ "$state" = "off" ]
   then
      echo "turn off led $gpio"
      sudo sh -c "echo 1 > /sys/class/gpio/$gpioFolder/value"
   else
      echo "input wrong state"
      exit 0
   fi
fi

# CONTROL LED 3
if [ -z "$led3" ]; then
   exit 0
else
   gpio=$led3
   gpioFolder="gpio$gpio"

   # Check that gpio folder existed
   temp=$(find /sys/class/gpio/ -maxdepth 1 -name $gpioFolder)
   if [ "$temp" != "/sys/class/gpio/$gpioFolder" ]; then
      sudo sh -c "echo $gpio > /sys/class/gpio/export"
      temp=$(find /sys/class/gpio/ -maxdepth 1 -name $gpioFolder)
      if [ "$temp" != "/sys/class/gpio/$gpioFolder" ]; then
         echo "can not control this gpio $gpio"
         exit 0 
      fi
   fi

   # Check current direction
   direct=$(cat /sys/class/gpio/$gpioFolder/direction)
   if [ "$direct" = "in" ]; then
      sudo sh -c "echo out > /sys/class/gpio/$gpioFolder/direction"
   fi

   # Turn on or turn off Led depend on input parameter
   if [ "$state" = "on" ]
   then
      echo "turn on  led $gpio"
      sudo sh -c "echo 0 > /sys/class/gpio/$gpioFolder/value"
   elif [ "$state" = "off" ]
   then
      echo "turn off led $gpio"
      sudo sh -c "echo 1 > /sys/class/gpio/$gpioFolder/value"
   else
      echo "input wrong state"
      exit 0
   fi
   exit 0
fi
