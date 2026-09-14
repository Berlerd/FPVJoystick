Rpi5 Graphics:
Install the SDL simple graphic lib:
sudo apt install libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core
copy two files into the project lib Projects/Joy:
CMakeLists.txt
Rpijoysdl.cpp

cmake --build build
sudo -E ./build/main_sdl /dev/ttyAMA0

----------------------------------------------------------------------------

firmware simulator 2.0 running the SD card:
C:/Users/xx/OneDrive - xx/Documents/EdgeTx/Companion/SD

inside the SD:
C:\Users\xx\OneDrive - xx\Documents\EdgeTx\Companion\SD\SCRIPTS\MIXES
Mode.lua
Land.lua



rpi instructions:
SBUS driver: https://github.com/Carbon225/raspberry-sbus/tree/master
nano CMakeLists.txt
cmake -B build -S . 
cmake --build build 
sudo ./build/main3 /dev/ttyAMA0


