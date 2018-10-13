//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////

#include "processor_container.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/list.h>

struct containerThreads
{
    struct task_struct* currentPtr;
    struct containerThreads* next;
};

struct container
{
    u64 cid;
    struct containerThreads* threadList;
    struct container* next;
    static DEFINE_MUTEX(perContainerLock);
};

struct container *containerListHead = NULL;
static DEFINE_MUTEX(containerListLock);

/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */
int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{
    struct processor_container_cmd my_cmd = {-1,-1};
    copy_from_user(&my_cmd, user_cmd, sizeof(struct processor_container_cmd));
 
    if(user_cmd == NULL)
    {
        printk(KERN_ERR "user_cmd is null\n");
        return 0;
    }

    mutex_lock(&containerListLock);
    struct container* containerPtr = containerListHead;
    if(containerListHead == NULL)
    {
        printk(KERN_ERR "No containers are present\n");
        mutex_unlock(&containerListLock);
        return 0;
    }

    struct container* prevContainerPtr = NULL;
    while(containerPtr->cid != my_cmd.cid)
    {
        prevContainerPtr = containerPtr;
        containerPtr = containerPtr->next;
    }

    if(containerPtr == NULL)
    {
        printk(KERN_ERR "container id %llu not present in present container list\n", my_cmd.cid);
        mutex_unlock(&containerListLock);
        return 0;
    }

    //container ptr is not null means container exists.
    //releasing the global mutex and locking the per container mutex
    mutex_unlock(&containerListLock);
    mutex_lock(&containerPtr->perContainerLock);

    struct containerThreads* threadPtr = containerPtr->threadList;
    if(threadPtr == NULL)
    {
        printk(KERN_ERR "Thread list in container id %llu is NULL. Returning without deleting\n", my_cmd.cid);
        mutex_unlock(&containerPtr->perContainerLock);
        return 0;
    }

    struct containerThreads* threadPtrPrevious = NULL;
    while(threadPtr->currentPtr->pid != current->pid)
    {
        threadPtrPrevious = threadPtr;
        threadPtr = threadPtr->next;
    }

    if(threadPtr == NULL)
    {
        printk(KERN_ERR "thread with pid %d not present in current container id %llu", current->pid, my_cmd.cid);
        mutex_unlock(&containerPtr->perContainerLock);
        return 0;
    }

    if(threadPtrPrevious != NULL)
    {
        threadPtrPrevious->next = threadPtr->next;
        mutex_unlock(&containerPtr->perContainerLock);
    }
    else
    {
        if(threadPtr->next != NULL)
        {
            containerPtr->threadList = threadPtr->next;
            printk(KERN_DEBUG "waking process pid = %d in container %llu", containerPtr->threadList->currentPtr->pid, containerPtr->cid);
            wake_up_process(containerPtr->threadList->currentPtr);
            mutex_unlock(&containerPtr->perContainerLock);
        }
        else
        {
            //need to delete the container too
            //therefore need the global lock here
            mutex_lock(&containerListLock);
            if(prevContainerPtr != NULL)
                prevContainerPtr->next = containerPtr->next;
            else
                containerListHead = containerPtr->next;
            mutex_unlock(&containerListLock);
            mutex_unlock(&containerPtr->perContainerLock);
            printk(KERN_DEBUG "deleting container %llu", containerPtr->cid);
            kfree(containerPtr);
        }
    }

    printk(KERN_DEBUG "deleting process pid = %d in container %llu", threadPtr->currentPtr->pid, my_cmd.cid);
    kfree(threadPtr);
    
    return 0;
}

/**
 * Create a task in the corresponding container.
 * external functions needed:
 * copy_from_user(), mutex_lock(), mutex_unlock(), set_current_state(), schedule()
 * 
 * external variables needed:
 * struct task_struct* current  
 */
int processor_container_create(struct processor_container_cmd __user *user_cmd)
{
    struct processor_container_cmd my_cmd = {-1,-1};
    copy_from_user(&my_cmd, user_cmd, sizeof(struct processor_container_cmd));

    if(user_cmd == NULL)
    {
        printk(KERN_ERR "user_cmd is null\n");
        return 0;
    }

    struct container *ptr = NULL;
    mutex_lock(&containerListLock);

    //finding pointer to container with given cid
    //if no container with given cid is present then ptr is null
    for(ptr = containerListHead; ptr != NULL; ptr=ptr->next)
    {
        if(ptr->cid == my_cmd.cid)
            break;
    }

    //if ptr is null then creating a new container with given cid
    if(ptr == NULL)
    {
        struct container *tempContainer = (struct container *) kmalloc(sizeof(struct container), GFP_KERNEL);
        tempContainer->cid = my_cmd.cid;
        tempContainer->threadList = (struct containerThreads *) kmalloc(sizeof(struct containerThreads), GFP_KERNEL);
        tempContainer->threadList->currentPtr = current;
        tempContainer->threadList->next = NULL;
        tempContainer->next = NULL;
        if(containerListHead == NULL)
        {
            printk(KERN_DEBUG "adding container - %llu at the head of the list, pid = %d\n", my_cmd.cid, current->pid);
            containerListHead = tempContainer;
        }
        else
        {
            printk(KERN_DEBUG "appending container - %llu, pid = %d\n", my_cmd.cid, current->pid);
            struct container *lastContainerPtr = containerListHead;
            while(lastContainerPtr->next != NULL)
                lastContainerPtr = lastContainerPtr->next;
            
            lastContainerPtr->next = tempContainer;
        }

        mutex_unlock(&containerListLock);
    }
    else
    {
        printk(KERN_DEBUG "adding in existing container: %llu, pid = %d \n", my_cmd.cid, current->pid);
        mutex_unlock(&containerListLock);
        mutex_lock(&ptr->perContainerLock);
        struct containerThreads *lastThread = ptr->threadList;
        if(lastThread == NULL)
        {
            printk(KERN_ERR "thread head in container %llu is null. Returning without adding thread.\n", my_cmd.cid);
            mutex_unlock(&containerListLock);
            return 0;
        }

        while(lastThread->next != NULL)
            lastThread = lastThread->next;
        lastThread->next = (struct containerThreads *) kmalloc(sizeof(struct containerThreads), GFP_KERNEL);
        lastThread->next->currentPtr = current;
        lastThread->next->next = NULL;
        mutex_unlock(&ptr->perContainerLock);
    }

    if(ptr != NULL)
    {
        //sleeping the process
        printk(KERN_DEBUG "sleeping process %d \n", current->pid);
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }
    
    return 0;
}

/**
 * switch to the next task in the next container
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), set_current_state(), schedule()
 */
int processor_container_switch(struct processor_container_cmd __user *user_cmd)
{
    struct processor_container_cmd my_cmd = {-1,-1};
    copy_from_user(&my_cmd, user_cmd, sizeof(struct processor_container_cmd));
    if(user_cmd == NULL)
    {
        printk(KERN_ERR "user_cmd is null\n");
        return 0;
    }

    mutex_lock(&containerListLock);
    if(containerListHead == NULL)
    {
        printk(KERN_ERR "Switch:: No containers are present\n");
        mutex_unlock(&containerListLock);
        return 0;
    }

    struct container *containerPtr = containerListHead;
    while(containerPtr != NULL && containerPtr->threadList->currentPtr->pid != current->pid)
        containerPtr = containerPtr->next;

    if(containerPtr == NULL)
    {
        printk(KERN_ERR "Current thread with pid %d is not present in head of any container\n", current->pid);
        mutex_unlock(&containerListLock);
        return 0;
    }

    //container is not null. 

    struct containerThreads *currentThreadPtr = containerPtr->threadList;
    //if this is the only thread in container so no need to sleep
    if(currentThreadPtr->next == NULL)
    {
        printk(KERN_DEBUG "Thread %d is the only thread in the container %llu. No need to switch.\n",
            currentThreadPtr->currentPtr->pid, containerPtr->cid);     
        mutex_unlock(&containerListLock);
        return 0;
    }
    else
    {
        containerPtr->threadList = currentThreadPtr->next;
        currentThreadPtr->next = NULL;
        struct containerThreads *tempPtr = containerPtr->threadList;
        while(tempPtr->next != NULL)
            tempPtr = tempPtr->next;
        tempPtr->next = currentThreadPtr;

        mutex_unlock(&containerListLock);
        //wakeup the first process in the current container
        printk(KERN_DEBUG "sleeping process %d and waking process %d. in container %llu\n",
            current->pid, containerPtr->threadList->currentPtr->pid, containerPtr->cid);
        wake_up_process(containerPtr->threadList->currentPtr);
    }

    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
    return 0;
}

/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int processor_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case PCONTAINER_IOCTL_CSWITCH:
        return processor_container_switch((void __user *)arg);
    case PCONTAINER_IOCTL_CREATE:
        return processor_container_create((void __user *)arg);
    case PCONTAINER_IOCTL_DELETE:
        return processor_container_delete((void __user *)arg);
    default:
        return -ENOTTY;
    }
}
