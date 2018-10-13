# Resource-Containers

## Introduction
Virtualizing system resources within OS kernel to provide abstraction other than processes and threads for resource allocation. Depending on the demand of applications, OS assigns different containers with different resources, each with its own scheduling policy to efficiently use its resources.
Project is implemented in C at kernel level.

## steps to compile, execute and test (or demo)
cd ~/Resource-Containers/kernel_module

make

sudo make install

sudo insmod processor_container.ko

cd /dev

sudo chmod 777 /dev/pcontainer

cd ~/Resource-Containers/library

make

sudo make install

cd ~/Resource-Containers/benchmark

make

./benchmark <number of containers> <threads in each container>
  
For e.g. for 3 containers with 2,3 and 5 threads each execute

./benchmark 3 2 3 5
