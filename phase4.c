#include <stdio.h>
#include <stdlib.h>
#include "phase1.h"
#include "phase2.h"
#include <string.h>
#include "phase3.h"
#include "phase4.h"
#include "phase3_usermode.h"

typedef struct SleepingProcess{
    int isAsleep; // 0 if the process is awake, 1 if its asleep
    int mailBoxID;
    int wakeUpTime; // clock cycle count when it will wake


} SleepingProcess;

//each is controlled by the control register and the status register
//device input() CPU reads the status register
//device output CPU writes to control register
//impossible for cpu to writer to status register or read control register
typedef struct Terminal{
    int cursorIndex; // index of the character we are currently on
    int write; //mailbox ID to prevent writing at the same time
    int driverResponded; // mailbox ID to not continue until the driver has responded that it has read a character
    char *writerBuffer;
    int writeLen;
} Terminal;


static SleepingProcess sleepTable[MAXPROC];
static int timePassed = 0; // clock cycles counted so far

static Terminal Terminals[4];


int clockHandler(void *arg){
    int status;
    while(1){
        waitDevice(USLOSS_CLOCK_DEV, 0, &status);
        timePassed++;

        for (int i = 0; i < MAXPROC; i++){
            SleepingProcess *curr = &sleepTable[i];
            if (curr->isAsleep && curr->wakeUpTime <= timePassed){
                curr->isAsleep = 0;
                MboxCondSend(curr->mailBoxID, NULL, 0);
            }
        }
    }
    return 0;

}



void kernelSleep(int seconds){
    SleepingProcess *spot = &sleepTable[getpid() % MAXPROC];
    spot->wakeUpTime = timePassed + seconds * 10;
    spot->isAsleep = 1;

    MboxRecv(spot->mailBoxID, NULL, 0);
}


void sleepHandler(USLOSS_Sysargs *args){
    int seconds = (int)(long)args->arg1;
    if (seconds < 0){
        args->arg4 = (void *)(long)-1;
        return;
    }
    kernelSleep(seconds);
    args->arg4 = (void *)(long)0;
}

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
    //receive branch


}

int terminalInterruptHandler(void *arg){
    int unit = (int)(long)arg;
    int status;
    while (1){
        waitDevice(USLOSS_TERM_DEV, unit, &status);
        handle_one_terminal_interrupt(unit, status);
    }
    return 0;

}

int kernelWrite(char* buffer, int bufferSize, int unitID){
    if (bufferSize <= 0 || unitID > 3 || unitID < 0){
        return 0;
    }

    Terminal *currTerminal = &Terminals[unitID];

    MboxSend(currTerminal->write, NULL, 0);

    currTerminal->writerBuffer = buffer;
    currTerminal->writeLen = bufferSize;
    currTerminal->cursorIndex = 0;

    int cr_val = 0x1;
    cr_val  |= 0x2;
    cr_val |= 0x4;
    cr_val |= ((unsigned char)currTerminal->writerBuffer[currTerminal->cursorIndex] << 8);
    currTerminal->cursorIndex++;
    USLOSS_DeviceOutput(USLOSS_TERM_DEV, unitID, (void*)(long)cr_val);

    MboxRecv(currTerminal->driverResponded, NULL, 0);

    MboxRecv(currTerminal->write, NULL, 0);
    return bufferSize;


}

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





void termReadHandler(USLOSS_Sysargs *args){
    args->arg2 = (void *)(long)0;
    args->arg4 = (void *)(long)-1;
}

void diskSizeHandler(USLOSS_Sysargs *args){

}

void diskReadHandler(USLOSS_Sysargs *args){

}

void diskWriterHandler(USLOSS_Sysargs *args){

}



void phase4_init() {

    for (int i = 0; i < MAXPROC; i++){
        sleepTable[i].isAsleep = 0;
        sleepTable[i].mailBoxID = MboxCreate(1, 0);
    }

    for (int i = 0; i < 4; i++){
        Terminals[i].write = MboxCreate(1,0);
        Terminals[i].driverResponded = MboxCreate(1, 0);
        Terminals[i].writerBuffer = NULL;
    }

        systemCallVec[SYS_SLEEP] = sleepHandler;
        systemCallVec[SYS_TERMREAD] = termReadHandler;
        systemCallVec[SYS_TERMWRITE] = termWriteHandler;

}

void phase4_start_service_processes(void){
    spork("clockHandler", clockHandler, NULL, USLOSS_MIN_STACK, 2);
    char processName[24];
    for (int i = 0; i < 4; i++){
        snprintf(processName, sizeof(processName), "Terminal%d", i);
        spork(processName, terminalInterruptHandler, (void *)(long)i, USLOSS_MIN_STACK, 2);
    }
}






