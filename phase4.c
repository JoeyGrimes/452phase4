/*
 * phase4.c
 * Author: Joe Grimes, Michael Masenheimer
 * This file implements the Phase 4 device drivers for the clock and the terminals,
 * Each device unit has its own driver process, which is the only process that calls waitDevice() on that unit.
 * The drivers talk to the syscall handlers through mailboxes: the clock driver wakes sleeping processes,
 * and each terminal driver sends output characters for TermWrite() and buffers input lines for TermRead().
*/

#include <stdio.h>
#include <stdlib.h>
#include "phase1.h"
#include "phase2.h"
#include <string.h>
#include "phase3.h"
#include "phase4.h"
#include "phase3_usermode.h"

typedef struct SleepingProcess{
    int mailBoxID; //1 slot mailbox the brocess blocks until the clock driver wakes it
    int wakeUpTime; // clock cycle count when it will wake
    struct SleepingProcess *next; // previous sleeping process in the chain of sleeping processes

} SleepingProcess;


typedef struct Terminal{
    int cursorIndex; // index of the character we are currently on
    int write; //mailbox ID to prevent writing at the same time
    int driverResponded; // mailbox ID to not continue until the driver has responded that it has read a character
    char *writerBuffer; // buffer currently being written, its NULL when no write is in progress
    int writeLen; //number of characters in the current write
    int lineMbox;//mbox id holding up to 10 finished input lines
    char lineBuf[MAXLINE];// this is the input line currently being typed
    int lineLen; // numbers of chars in lineBuf so far
} Terminal;

static SleepingProcess sleepTable[MAXPROC];//1 entry per process slot index by pid%MACPROC
static int timePassed = 0; // clock cycles counted so far
static SleepingProcess *sleepHead = NULL;
static Terminal Terminals[4];

/*
 * Name: clockHandler
 * purpose: Clock device driver process. Loops forever calling waitDevice() on the clock
 * Each time it returns the tick count goes up by one, and any sleeping process
 * whose wake up time has been reached is woken by sending to its mailbox
 * Params: void *arg is unused, just put here for the signature
 */
int clockHandler(void *arg){
    int status;
    while(1){
        waitDevice(USLOSS_CLOCK_DEV, 0, &status);
        timePassed++;

        SleepingProcess *prev = NULL;
        SleepingProcess *curr = sleepHead;
        while (curr != NULL){
            SleepingProcess *next = curr->next;
            if (curr->wakeUpTime <= timePassed){
                if (prev == NULL){
                    sleepHead = next;
                } else {
                    prev->next = next;
                }
                curr->next = NULL;
                MboxCondSend(curr->mailBoxID, NULL, 0);
            } else {
                prev = curr;
            }
            curr = next;
        }


    }
    return 0;
}

/*
 * Name: kernelSleep
 * Purpose: This method puts the calling process to sleep for a number of seconds. It records the tick the process
 * should wake at (10 ticks per second), marks it asleep, and blocks on its mailbox until the clock driver wakes it.
 * wakeUpTime is set before isAsleep so the driver never sees a sleeping entry with an old wake time. If the wake up
 * is sent before we reach MboxRecv(), it waits in the mailbox's slot, so it is never lost.
 * Params: int seconds, the number of seconds to sleep
 */
void kernelSleep(int seconds){
    SleepingProcess *spot = &sleepTable[getpid() % MAXPROC];
    spot->wakeUpTime = timePassed + seconds * 10;
    spot->next = sleepHead;
    sleepHead = spot;

    MboxRecv(spot->mailBoxID, NULL, 0);
}

/*
 * Name: sleepHandler
 * purpose: syscall handler for SYS_SLEEP. 
 * It reads the number of seconds from arg1
 * checks that it is not negative, and sleeps the calling process.
 * Params: USLOSS_Sysargs *args, arg1 is the number of seconds
 * Returns: nothing but arg4 is set to -1 if the seconds are negative, 0 otherwise
 */
void sleepHandler(USLOSS_Sysargs *args){
    int seconds = (int)(long)args->arg1;
    if (seconds < 0){
        args->arg4 = (void *)(long)-1;
        return;
    }
    kernelSleep(seconds);
    args->arg4 = (void *)(long)0;
}

/*
 * Name: handle_one_terminal_interrupt
 * Purpose: handles one status value from a terminal. if a write is in progress
 * and the terminal is ready, it sends the next character, or if every 
 * character has been sent, it ends the write and wakes the writer. 
 * If a character arrived, it is added to the current line. A finished
 * line is sent to the terminal's line mailbox. Any lines past the 10th are dropped. 
 * Params: int unit, the terminal unit (0-3) andint status, the status 
 * register value returned by waitDevice()
 */
void handle_one_terminal_interrupt(int unit, int status){
    Terminal *currTerminal = &Terminals[unit];
    if (USLOSS_TERM_STAT_XMIT(status) == USLOSS_DEV_READY && currTerminal->writerBuffer != NULL){
        if (currTerminal->cursorIndex == currTerminal->writeLen){
            currTerminal->writerBuffer = NULL;
            currTerminal->writeLen = 0;
            MboxCondSend(currTerminal->driverResponded, NULL, 0);
        }
        else{
            int cr_val = 0x1;
            cr_val  |= 0x2;
            cr_val |= 0x4;
            cr_val |= ((unsigned char)currTerminal->writerBuffer[currTerminal->cursorIndex] << 8);

            USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void*)(long)cr_val);
            currTerminal->cursorIndex++;
        }
    }

    if (USLOSS_TERM_STAT_RECV(status) == USLOSS_DEV_BUSY) {
        char theChr = USLOSS_TERM_STAT_CHAR(status);
        currTerminal->lineBuf[currTerminal->lineLen] = theChr;
        currTerminal->lineLen++;

        if (theChr == '\n' || currTerminal->lineLen == MAXLINE) {
            MboxCondSend(currTerminal->lineMbox, currTerminal->lineBuf, currTerminal->lineLen);
            currTerminal->lineLen = 0;
        }
    }
}

/*
 * name: terminalInterruptHandler
 * purpose: This method is the device driver process for one terminal. 
 * It first turns on receive and transmit interrupts for its terminal
 * then loops forever calling waitDevice() and handling each status it gets
 * Params: void *arg, the terminal unit which is (0-3)
 */
int terminalInterruptHandler(void *arg){
    int unit = (int)(long)arg;
    int status;
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unit, (void*)(long)(0x2 | 0x4));
    while (1){
        waitDevice(USLOSS_TERM_DEV, unit, &status);
        handle_one_terminal_interrupt(unit, status);
    }
    return 0;
}

/*
 * Name: kernelWrite
 * Purpose: Writes a buffer to a terminal atomically. It takes 
 * the terminal's write lock so no other write can start, sends the 
 * first character itself and only then hands the buffer to the driver
 * It blocks until the driver reports that every character has been sent
 * Params: char *buffer, the chars to write and int bufferSize, the number of chars to write
 * and int unitID, the terminal unit which can be(0-3)
 * Returns: The number of characters written, or 0 if nothing was to be written
 */
int kernelWrite(char* buffer, int bufferSize, int unitID){
    if (bufferSize <= 0 || unitID > 3 || unitID < 0){
        return 0;
    }

    Terminal *currTerminal = &Terminals[unitID];

    MboxSend(currTerminal->write, NULL, 0);

    int cr_val = 0x1;
    cr_val  |= 0x2;
    cr_val |= 0x4;
    cr_val |= ((unsigned char)buffer[0] << 8);


    currTerminal->writeLen = bufferSize;
    currTerminal->cursorIndex = 1;
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unitID, (void*)(long)cr_val);
    currTerminal->writerBuffer = buffer;

    MboxRecv(currTerminal->driverResponded, NULL, 0);

    MboxRecv(currTerminal->write, NULL, 0);
    return bufferSize;
}

/*
 * Name: termWriteHandler
 * Purpose:yscall handler for SYS_TERMWRITE. It reads and validates 
 * the arguments and writes the buffer to the terminal.
 * Params: USLOSS_Sysargs *args, arg1 is the buffer, 
 * arg2 is its length, arg3 is the terminal unit
 * Returns: Nothing, but arg2 is set to the number of 
 * characters written, arg4 is set to -1 if the arguments are
 * invalid, 0 otherwise
 */
void termWriteHandler(USLOSS_Sysargs *args){
    char *buffer   = (char *)args->arg1;
    int bufferSize = (int)(long)args->arg2;
    int unitID     = (int)(long)args->arg3;

    if (buffer == NULL || bufferSize < 0 || unitID < 0 || unitID > 3){
        args->arg2 = (void *)(long)0;
        args->arg4 = (void *)(long)-1;
        return;
    }

    int written = kernelWrite(buffer, bufferSize, unitID);
    args->arg2 = (void *)(long)written;
    args->arg4 = (void *)(long)0;
}

/*
 * Name: kernelRead
 * purpose: reads one line from a terminal and blocks until a full line 
 * is available, receiving it into a full size buffer first, Then it copies 
 * as many characters as fit into the caller's buffer
 * Params: char *buffer, where to copy the line, int bufferSize, the size of the caller's 
 * buffer and int unitID, the terminal unit which can be(0-3)
 * Returns: The number of characters copied into buffer
 */
int kernelRead(char *buffer, int bufferSize, int unitID) {
    //recieve into a full linesize first, because maybe the caller's buffer is smaller

    char line[MAXLINE];
    int len = MboxRecv(Terminals[unitID].lineMbox, line, MAXLINE);
    if (len < 0) {
        return 0;
    }
    
    if (len > bufferSize) {
        len = bufferSize;
    }
    memcpy(buffer, line, len);
    return len;
}

/*
 * name: termReadHandler
 * Purpose: syscall handler for SYS_TERMREAD. It reads 
 * and validatesa the arguments and reads one line from the terminal.
 * params: USLOSS_Sysargs *args, arg1 is the buffer, arg2 is its size, arg3 is the terminal unit
 * Returns: nothign, but arg2 is set to the number of characters read, arg4 is set to -1 if
 *  the arguments are invalid 0 otherwise
 */
void termReadHandler(USLOSS_Sysargs *args){
    char *buffer = (char *)args->arg1;
    int bufferSize = (int)(long)args->arg2;
    int unitID = (int)(long)args->arg3;

    if (buffer == NULL || bufferSize <= 0 || unitID < 0 || unitID > 3) {
        args->arg2 = (void *)(long)0;
        args->arg4 = (void *)(long)-1;
        return;
    }

    int read = kernelRead(buffer, bufferSize, unitID);
    args->arg2 = (void *)(long)read;
    args->arg4 = (void *)(long)0;
}


void diskSizeHandler(USLOSS_Sysargs *args){
    // other part of phase 4 here
}

void diskReadHandler(USLOSS_Sysargs *args){
    // other part of phase 4 here
}

void diskWriterHandler(USLOSS_Sysargs *args){
    // other part of phase 4 here
}

/*
 * Name: phase4_init
 * Purpose: sets up Phase 4 before any processes run. 
 * It creates a sleep mailbox for every process slot, creates the write 
 * lock, write done, and input line mailboxes for each terminal
 */
void phase4_init() {

    for (int i = 0; i < MAXPROC; i++){
        sleepTable[i].mailBoxID = MboxCreate(1, 0);
    }

    for (int i = 0; i < 4; i++){
        Terminals[i].write = MboxCreate(1,0);
        Terminals[i].driverResponded = MboxCreate(1, 0);
        Terminals[i].writerBuffer = NULL;
        Terminals[i].lineMbox = MboxCreate(10, MAXLINE);
        Terminals[i].lineLen = 0;
    }

        systemCallVec[SYS_SLEEP] = sleepHandler;
        systemCallVec[SYS_TERMREAD] = termReadHandler;
        systemCallVec[SYS_TERMWRITE] = termWriteHandler;
}

/*
 * Name: phase4_start_service_processes
 * Purpose: starts the Phase 4 device driver processes, 
 * one for the clock and one for each of the 4 terminals
 * They run at priority 2, so a driver runs ahead of the user process
 */
void phase4_start_service_processes(void){
    spork("clockHandler", clockHandler, NULL, USLOSS_MIN_STACK, 2);
    char processName[24];
    for (int i = 0; i < 4; i++){
        snprintf(processName, sizeof(processName), "Terminal%d", i);
        spork(processName, terminalInterruptHandler, (void *)(long)i, USLOSS_MIN_STACK, 2);
    }
}

