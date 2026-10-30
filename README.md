# logging file-fork
Logging tool that is created to install the file added here and run upon reboot to send logs to the PICO setup

Download the .sh file and launch with sudo and set the [-m] to select which mode of the logger is to run. Set additional flags as per the mode selected, this will start the service and pin it to restart on reboots. Currently working on not eliminating the previous pin when creating a new one. Will create a clean all script once the names have been standardised.
